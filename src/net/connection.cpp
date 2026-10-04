#include <ember/net/connection.h>

#include <algorithm>
#include <cmath>

namespace ember::net
{
	namespace
	{
		constexpr f64 ACK_DELAY_UNIT	= 0.0005; // seconds per step: half a millisecond
		constexpr u32 ACK_DELAY_UNKNOWN = 255;	  // waited too long to say; no round trip sample from it

		constexpr f32 LOSS_SMOOTHING = 1.0f / 64.0f;
		constexpr f64 RATE_PERIOD	 = 0.25; // seconds of traffic per rate measurement

		struct Header
		{
			Sequence sequence = 0;
			bool has_ack	  = false;
			Sequence ack	  = 0;
			u32 ack_bits	  = 0;
			u32 ack_delay	  = 0;

			template <class Stream> bool serialize(Stream& stream)
			{
				serialize_bits(stream, sequence, 16);
				serialize_bool(stream, has_ack);

				if (has_ack)
				{
					serialize_bits(stream, ack, 16);
					serialize_bits(stream, ack_bits, 32);
					serialize_bits(stream, ack_delay, 8);
				}

				return true;
			}
		};
	}

	Connection::Connection(const ConnectionDef& def, f64 now) noexcept : m_def(def) { reset(now); }

	void Connection::reset(f64 now) noexcept
	{
		m_stats			 = {};
		m_send_times	 = {};
		m_next_sequence	 = 0;
		m_oldest_pending = 0;
		m_has_received	 = false;
		m_received		 = 0;
		m_received_bits	 = 0;
		m_received_time	 = now;
		m_last_heard	 = now;
		m_rtt_window	 = {};
		m_has_rtt		 = false;
		m_send_meter	 = {.start = now};
		m_receive_meter	 = {.start = now};
		m_notices.clear();
	}

	Sequence Connection::write_header(serialize::WriteStream& stream, f64 now) noexcept
	{
		// A full window means the oldest packet has gone a whole window (four seconds at 60 Hz) with no
		// news either way: give it up, so its slot can hold the new one and the layers above can act.
		if (in_flight() == WINDOW)
			settle_oldest(false);

		const Sequence sequence			= m_next_sequence++;
		m_send_times[sequence % WINDOW] = now;
		++m_stats.packets_sent;

		Header header = {.sequence = sequence, .has_ack = m_has_received};

		if (m_has_received)
		{
			const f64 waited = std::round((now - m_received_time) / ACK_DELAY_UNIT);

			header.ack		 = m_received;
			header.ack_bits	 = m_received_bits;
			header.ack_delay = waited < 0.0					? 0u
							   : waited < ACK_DELAY_UNKNOWN ? static_cast<u32>(waited)
															: ACK_DELAY_UNKNOWN;
		}

		[[maybe_unused]] const bool written = header.serialize(stream);
		EMBER_ASSERT(written);

		return sequence;
	}

	void Connection::count_sent(u32 bytes, f64 now) noexcept
	{
		m_stats.bytes_sent += bytes;
		m_send_meter.add(bytes, now, m_stats.send_rate);
	}

	Connection::Arrival Connection::read_header(serialize::ReadStream& stream, Sequence& sequence, f64 arrival) noexcept
	{
		Header header;
		const bool read = header.serialize(stream);

		// Acking a packet this end has not sent yet is impossible for an honest peer.
		const bool ack_sent = !header.has_ack || sequence_delta(header.ack, m_next_sequence) < 0;

		if (!read || !ack_sent)
		{
			++m_stats.packets_malformed;
			return Arrival::Malformed;
		}

		if (header.has_ack)
			take_acks(header.ack, header.ack_bits, header.ack_delay, arrival);

		if (m_has_received && !sequence_newer(header.sequence, m_received))
		{
			++m_stats.packets_stale;
			return Arrival::Stale;
		}

		sequence = header.sequence;
		return Arrival::Fresh;
	}

	void Connection::acknowledge(Sequence sequence, u32 bytes, f64 arrival) noexcept
	{
		EMBER_ASSERT((!m_has_received || sequence_newer(sequence, m_received)) && "acknowledge Fresh packets only");

		if (m_has_received)
		{
			// The previous newest becomes bit (shift - 1); anything shifted past bit 31 is forgotten.
			// A hostile peer can jump thousands ahead, so the shift is checked before it is made.
			const i32 shift = sequence_delta(sequence, m_received);

			m_received_bits =
				shift > 32 ? 0u
						   : static_cast<u32>((static_cast<u64>(m_received_bits) << shift) | (u64{1} << (shift - 1)));

			// Whatever it passed over will never be applied, in order or not at all: lost, to this end, as
			// the peer will count it once the acks say so. Each counts toward the loss as one packet would.
			const u32 skipped = static_cast<u32>(shift - 1);
			m_stats.packets_skipped += skipped;
			m_stats.receive_loss =
				1.0f - (1.0f - m_stats.receive_loss) * std::pow(1.0f - LOSS_SMOOTHING, static_cast<f32>(skipped));
		}

		m_has_received	= true;
		m_received		= sequence;
		m_received_time = arrival;
		m_last_heard	= std::max(m_last_heard, arrival);

		++m_stats.packets_received;
		m_stats.bytes_received += bytes;
		m_stats.receive_loss *= 1.0f - LOSS_SMOOTHING;
		m_receive_meter.add(bytes, arrival, m_stats.receive_rate);
	}

	bool Connection::take_notice(PacketNotice& notice) noexcept
	{
		if (m_notices.empty())
			return false;

		notice = m_notices.front();
		m_notices.pop();
		return true;
	}

	bool Connection::timed_out(f64 now) const noexcept { return now - m_last_heard > m_def.timeout; }

	void Connection::take_acks(Sequence ack, u32 ack_bits, u32 ack_delay, f64 arrival) noexcept
	{
		// Everything up to the ack is settled now. Older acks than that were settled long ago.
		while (in_flight() > 0 && !sequence_newer(m_oldest_pending, ack))
		{
			const Sequence sequence = m_oldest_pending;
			const i32 age			= sequence_delta(ack, sequence); // 0 is the ack itself

			const bool delivered = age == 0 || (age <= 32 && ((ack_bits >> (age - 1)) & 1) != 0);

			// Only the ack itself gives a round trip: its delay is the one the peer reported.
			if (age == 0 && ack_delay != ACK_DELAY_UNKNOWN)
			{
				const f64 rtt = arrival - m_send_times[sequence % WINDOW] - ack_delay * ACK_DELAY_UNIT;
				sample_rtt(std::max(rtt, 0.0), arrival);
			}

			settle_oldest(delivered);
		}
	}

	void Connection::settle_oldest(bool delivered) noexcept
	{
		EMBER_ASSERT(in_flight() > 0);
		EMBER_ASSERT(!m_notices.full() && "take every notice each update");

		m_notices.push(PacketNotice{.sequence = m_oldest_pending, .delivered = delivered});
		++m_oldest_pending;

		if (delivered)
			++m_stats.packets_delivered;
		else
			++m_stats.packets_lost;

		m_stats.loss += ((delivered ? 0.0f : 1.0f) - m_stats.loss) * LOSS_SMOOTHING;
	}

	void Connection::sample_rtt(f64 rtt, f64 now) noexcept
	{
		ConnectionStats& s = m_stats;

		if (!m_has_rtt)
		{
			s.rtt		   = rtt;
			s.rtt_variance = rtt * 0.5;
			m_has_rtt	   = true;
		}
		else
		{
			s.rtt_variance += (std::abs(s.rtt - rtt) - s.rtt_variance) * 0.25;
			s.rtt += (rtt - s.rtt) * 0.125;
		}

		s.rtt_latest = rtt;

		// The minimum over ten one second buckets: queues come and go, the path's floor stays.
		const i64 second   = static_cast<i64>(std::floor(now));
		MinBucket& current = m_rtt_window[static_cast<u64>(second) % m_rtt_window.size()];

		if (current.second != second)
			current = {.second = second, .min = rtt};
		else
			current.min = std::min(current.min, rtt);

		f64 floor = rtt;
		for (const MinBucket& bucket : m_rtt_window)
		{
			if (bucket.second > second - static_cast<i64>(m_rtt_window.size()))
				floor = std::min(floor, bucket.min);
		}

		s.rtt_min = floor;
	}

	void Connection::RateMeter::add(u32 count, f64 now, f32& rate) noexcept
	{
		bytes += count;

		const f64 elapsed = now - start;
		if (elapsed < RATE_PERIOD)
			return;

		const f32 measured = static_cast<f32>(static_cast<f64>(bytes) / elapsed);
		rate			   = rate == 0.0f ? measured : rate + (measured - rate) * 0.5f;
		bytes			   = 0;
		start			   = now;
	}
}
