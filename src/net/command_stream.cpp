#include <ember/net/command_stream.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ember::net
{
	namespace
	{
		constexpr u32 VIEW_BITS		  = 12; // sixteenths of a tick, up to 256 ticks behind
		constexpr u32 VIEW_MAX		  = (1u << VIEW_BITS) - 1;
		constexpr f32 SIXTEENTHS	  = 16.0f; // view delays and timing travel in sixteenths of a tick
		constexpr i32 TIMING_MIN	  = -2048; // sixteenths of a tick: 128 ticks either way
		constexpr i32 TIMING_MAX	  = 2047;
		constexpr i32 DEVIATION_MAX	  = 255;
		constexpr f32 DEVIATION_PRIOR = 0.5f; // assumed spread until commands say otherwise, in ticks
		constexpr u32 PACKET_BITS	  = MAX_PACKET_BYTES * 8;

		[[nodiscard]] u16 encode_view(f32 view_delay) noexcept
		{
			const f32 scaled = std::isfinite(view_delay) ? std::round(view_delay * SIXTEENTHS) : 0.0f;
			return static_cast<u16>(std::clamp(scaled, 0.0f, static_cast<f32>(VIEW_MAX)));
		}

		[[nodiscard]] i32 sixteenths(f32 ticks, i32 min, i32 max) noexcept
		{
			const f32 scaled = std::isfinite(ticks) ? std::round(ticks * SIXTEENTHS) : 0.0f;
			return static_cast<i32>(std::clamp(scaled, static_cast<f32>(min), static_cast<f32>(max)));
		}

		/** One tick of a packet's command section. */
		struct Entry
		{
			bool present							= false; // clear for a tick the client's clock jumped over
			u16 view								= 0;	 // sixteenths of a tick
			std::array<u8, MAX_COMMAND_BYTES> bytes = {};
		};

		/**
		 * A packet's command section. One function reads, writes and measures it, so the three cannot
		 * disagree:
		 *
		 *   epoch       8  the client clock the commands were made on
		 *   count       6  ticks covered, 0 to 32
		 *   first      32  the oldest tick covered; only when count is not 0
		 *   per tick:
		 *     present   1  clear for a tick the clock jumped over, and then nothing follows
		 *     view     12  the view delay, in sixteenths of a tick
		 *     command   …  the codec's: one bit when it repeats the command before it
		 */
		struct Section
		{
			const CommandCodec& codec;
			Entry* entries = nullptr; // count of them, the first for tick `first`
			u8 epoch	   = 0;
			u32 count	   = 0;
			Tick first	   = NO_TICK;

			template <class Stream> bool serialize(Stream& stream) noexcept
			{
				serialize_bits(stream, epoch, 8);
				serialize_int(stream, count, 0, MAX_COMMANDS_PER_PACKET);

				if (count == 0)
					return true;

				serialize_bits(stream, first, 32);

				// Ticks start at 1, and the last one covered must not wrap past the end of time.
				if (Stream::IsReading && (first == NO_TICK || first > std::numeric_limits<Tick>::max() - (count - 1)))
					return serialize::serialize_fail(stream);

				const u8* previous = nullptr;
				for (u32 i = 0; i < count; ++i)
				{
					Entry& entry = entries[i];
					serialize_bool(stream, entry.present);

					if (!entry.present)
						continue;

					serialize_bits(stream, entry.view, VIEW_BITS);

					if (!command(stream, entry.bytes.data(), previous))
						return false;

					previous = entry.bytes.data();
				}

				return true;
			}

			template <class Stream> bool command(Stream& stream, u8* bytes, const u8* previous) const noexcept
			{
				if constexpr (Stream::IsReading)
					return codec.read(stream, bytes, previous);
				else if constexpr (std::is_same_v<Stream, serialize::MeasureStream>)
					return codec.measure(stream, bytes, previous);
				else
					return codec.write(stream, bytes, previous);
			}
		};

		[[nodiscard]] u32 measure(Section& section) noexcept
		{
			serialize::MeasureStream stream;
			[[maybe_unused]] const bool measured = section.serialize(stream);
			EMBER_ASSERT(measured);
			return static_cast<u32>(stream.GetBitsProcessed());
		}

		/** The server's report on command timing: a flag, then the timing in sixteenths of a tick. */
		struct TimingReport
		{
			bool present  = false;
			u8 epoch	  = 0;
			i32 mean	  = 0;
			i32 deviation = 0;

			template <class Stream> bool serialize(Stream& stream) noexcept
			{
				serialize_bool(stream, present);

				if (!present)
					return true;

				serialize_bits(stream, epoch, 8);
				serialize_int(stream, mean, TIMING_MIN, TIMING_MAX);
				serialize_int(stream, deviation, 0, DEVIATION_MAX);
				return true;
			}
		};
	}

	CommandSender::CommandSender(const CommandCodec& codec) noexcept : m_codec(codec)
	{
		EMBER_ASSERT(codec.size > 0 && codec.size <= MAX_COMMAND_BYTES);
		EMBER_ASSERT(codec.write != nullptr && codec.measure != nullptr && codec.read != nullptr);
	}

	void CommandSender::reset() noexcept
	{
		m_commands.clear();
		m_sent		   = {};
		m_newest	   = NO_TICK;
		m_acknowledged = NO_TICK;
	}

	void CommandSender::push_bytes(Tick tick, const void* command, f32 view_delay) noexcept
	{
		EMBER_ASSERT(tick != NO_TICK);

		// A tick at or before the newest was sent already, before the clock jumped back. The server
		// keeps the first command it got for a tick, so the recorded one stands; replay it from find().
		if (m_newest != NO_TICK && tick <= m_newest)
			return;

		Record& record = m_commands.write(tick);
		record.view	   = encode_view(view_delay);
		std::memcpy(record.bytes.data(), command, m_codec.size);

		m_newest = tick;
	}

	void CommandSender::write(serialize::WriteStream& stream, Sequence packet, u8 epoch, u32 reserve_bits) noexcept
	{
		std::array<Entry, MAX_COMMANDS_PER_PACKET> entries;
		Section section = {.codec = m_codec, .entries = entries.data(), .epoch = epoch};

		// Everything the server may still lack, capped to the newest commands, starting at a tick that
		// has one: leading ticks the clock jumped over say nothing.
		if (m_newest != NO_TICK && m_newest > m_acknowledged)
		{
			Tick first = m_acknowledged + 1;
			if (m_newest - first + 1 > MAX_COMMANDS_PER_PACKET)
				first = m_newest - MAX_COMMANDS_PER_PACKET + 1;

			while (first < m_newest && !m_commands.contains(first))
				++first;

			section.first = first;
			section.count = m_newest - first + 1;

			for (u32 i = 0; i < section.count; ++i)
			{
				const Record* record = m_commands.find(first + i);
				entries[i].present	 = record != nullptr;

				if (record != nullptr)
				{
					entries[i].view	 = record->view;
					entries[i].bytes = record->bytes;
				}
			}
		}

		// Commands are small and a packet of them rarely near full, but it must never overflow: leave
		// out the oldest until the rest fits. The newest matter most; their ticks are still to run.
		const u32 used	 = static_cast<u32>(stream.GetBitsProcessed()) + reserve_bits;
		const u32 budget = used < PACKET_BITS ? PACKET_BITS - used : 0;

		while (section.count > 0 && measure(section) > budget)
		{
			do
			{
				++section.entries;
				++section.first;
				--section.count;
			} while (section.count > 0 && !section.entries[0].present);
		}

		EMBER_ASSERT(measure(section) <= budget && "no room left in the packet for a command section");

		m_sent[packet % m_sent.size()] = {.packet = packet, .valid = section.count > 0, .newest = m_newest};

		[[maybe_unused]] const bool written = section.serialize(stream);
		EMBER_ASSERT(written);
	}

	void CommandSender::on_notice(const PacketNotice& notice) noexcept
	{
		Sent& sent = m_sent[notice.sequence % m_sent.size()];

		if (!sent.valid || sent.packet != notice.sequence)
			return;

		// Everything up to the packet's newest command is acknowledged. Older ones it did not carry were
		// delivered by an earlier packet, or left out as the oldest of more than a packet holds; either
		// way they are not sent again.
		if (notice.delivered)
			m_acknowledged = std::max(m_acknowledged, sent.newest);

		sent.valid = false;
	}

	bool CommandSender::find_bytes(Tick tick, void* out) const noexcept
	{
		const Record* record = m_commands.find(tick);
		if (record == nullptr)
			return false;

		std::memcpy(out, record->bytes.data(), m_codec.size);
		return true;
	}

	CommandQueue::CommandQueue(const CommandCodec& codec, const CommandQueueDef& def) noexcept
		: m_codec(codec), m_def(def)
	{
		EMBER_ASSERT(codec.size > 0 && codec.size <= MAX_COMMAND_BYTES);
		EMBER_ASSERT(codec.write != nullptr && codec.measure != nullptr && codec.read != nullptr);
		EMBER_ASSERT(def.tick_seconds > 0.0);
	}

	void CommandQueue::reset() noexcept
	{
		m_slots.clear();
		m_last_taken = NO_TICK;
		m_newest	 = NO_TICK;
		m_has_last	 = false;
		m_has_carry	 = false;
		m_has_epoch	 = false;
		m_last_view	 = 0;
		m_timing	 = {};
		m_samples	 = 0;
		m_misses	 = 0;
	}

	bool CommandQueue::read(serialize::ReadStream& stream, f64 arrival) noexcept
	{
		std::array<Entry, MAX_COMMANDS_PER_PACKET> entries;
		Section section = {.codec = m_codec, .entries = entries.data()};

		if (!section.serialize(stream))
			return false;

		// A newer epoch is a client clock that just jumped: what was measured before describes a
		// clock that no longer exists, so measuring starts over. An older one is a straggler from
		// before the jump; its commands still count, its timing does not.
		if (!m_has_epoch || static_cast<i8>(static_cast<u8>(section.epoch - m_timing.epoch)) > 0)
		{
			m_timing	= {.epoch = section.epoch};
			m_samples	= 0;
			m_has_epoch = true;
		}

		for (u32 i = 0; i < section.count; ++i)
		{
			if (entries[i].present)
				store(section.first + i, entries[i].bytes.data(), entries[i].view, section.epoch, arrival);
		}

		return true;
	}

	void CommandQueue::store(Tick tick, const u8* bytes, u16 view, u8 epoch, f64 arrival) noexcept
	{
		const bool current_epoch = epoch == m_timing.epoch;

		if (m_last_taken != NO_TICK && tick <= m_last_taken)
		{
			// Its tick already ran on a stand-in. Measure how late it was and keep its presses for the
			// next tick; a second late copy changes nothing.
			Slot* slot = m_slots.find(tick);
			if (slot == nullptr || !slot->taken_without || slot->has_command)
				return;

			slot->has_command = true;

			if (current_epoch)
				sample(static_cast<f32>(-(arrival - slot->taken) / m_def.tick_seconds));

			if (m_codec.merge_late != nullptr)
			{
				if (m_has_carry)
					m_codec.merge_late(m_carry.data(), bytes);
				else
					std::memcpy(m_carry.data(), bytes, m_codec.size);

				m_has_carry = true;
			}

			return;
		}

		// Further ahead than the queue holds: a broken or hostile clock. Taking it would evict ticks
		// still to run.
		if (m_last_taken != NO_TICK && tick - m_last_taken > COMMAND_HISTORY)
			return;

		// The first copy of a command wins; redundant copies in later packets are the norm.
		if (const Slot* existing = m_slots.find(tick); existing != nullptr && existing->has_command)
			return;

		Slot& slot		   = m_slots.write(tick);
		slot.arrival	   = arrival;
		slot.taken		   = 0.0;
		slot.view		   = view;
		slot.epoch		   = epoch;
		slot.has_command   = true;
		slot.taken_without = false;
		std::memcpy(slot.bytes.data(), bytes, m_codec.size);

		m_newest = std::max(m_newest, tick);
	}

	TakenCommand CommandQueue::take_bytes(Tick tick, void* out, f64 now) noexcept
	{
		EMBER_ASSERT(tick != NO_TICK);
		EMBER_ASSERT((m_last_taken == NO_TICK || tick > m_last_taken) && "take each tick once, in order");

		m_last_taken = tick;

		TakenCommand taken;
		Slot* slot = m_slots.find(tick);

		if (slot != nullptr && slot->has_command)
		{
			std::memcpy(out, slot->bytes.data(), m_codec.size);
			slot->taken = now;

			taken.source = CommandSource::Received;
			m_last_view	 = slot->view;

			if (slot->epoch == m_timing.epoch)
				sample(static_cast<f32>((now - slot->arrival) / m_def.tick_seconds));
		}
		else
		{
			// Nothing in time: run the tick on the last command, made safe to repeat, and remember
			// when, so the real one is measured as late if it ever turns up.
			Slot& missing		  = slot != nullptr ? *slot : m_slots.write(tick);
			missing.has_command	  = false;
			missing.taken_without = true;
			missing.taken		  = now;

			if (m_has_last)
			{
				std::memcpy(out, m_last.data(), m_codec.size);
				if (m_codec.repeat != nullptr)
					m_codec.repeat(out);

				taken.source = CommandSource::Repeated;
			}
			else
			{
				std::memcpy(out, m_codec.empty.data(), m_codec.size);
				taken.source = CommandSource::Empty;
			}

			++m_misses;
		}

		if (m_has_carry)
		{
			m_codec.merge_late(out, m_carry.data());
			m_has_carry = false;
		}

		std::memcpy(m_last.data(), out, m_codec.size);
		m_has_last		 = true;
		taken.view_delay = static_cast<f32>(m_last_view) / SIXTEENTHS;

		return taken;
	}

	void CommandQueue::sample(f32 slack) noexcept
	{
		CommandTiming& t = m_timing;

		if (m_samples++ == 0)
		{
			t.mean		= slack;
			t.deviation = DEVIATION_PRIOR;
			return;
		}

		t.deviation += (std::abs(slack - t.mean) - t.deviation) * m_def.deviation_smoothing;
		t.mean += (slack - t.mean) * m_def.mean_smoothing;
	}

	void write_command_timing(serialize::WriteStream& stream, const CommandQueue& queue) noexcept
	{
		TimingReport report = {.present = queue.has_timing()};

		if (report.present)
		{
			const CommandTiming& timing = queue.timing();

			report.epoch	 = timing.epoch;
			report.mean		 = sixteenths(timing.mean, TIMING_MIN, TIMING_MAX);
			report.deviation = sixteenths(timing.deviation, 0, DEVIATION_MAX);
		}

		[[maybe_unused]] const bool written = report.serialize(stream);
		EMBER_ASSERT(written);
	}

	bool read_command_timing(serialize::ReadStream& stream, CommandTiming& out, bool& present) noexcept
	{
		present = false;

		TimingReport report;
		if (!report.serialize(stream))
			return false;

		present = report.present;

		if (present)
		{
			out = {.epoch	  = report.epoch,
				   .mean	  = static_cast<f32>(report.mean) / SIXTEENTHS,
				   .deviation = static_cast<f32>(report.deviation) / SIXTEENTHS};
		}

		return true;
	}
}
