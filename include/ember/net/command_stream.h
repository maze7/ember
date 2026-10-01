#pragma once

#include <ember/net/connection.h>
#include <ember/net/serialize.h>
#include <ember/net/tick.h>

#include <array>
#include <concepts>
#include <cstring>
#include <type_traits>

namespace ember::net
{
	/** The largest command the stream carries: one tick of one client's input, sticks and buttons. */
	inline constexpr u32 MAX_COMMAND_BYTES = 64;

	/** Ticks of commands kept at each end: two seconds at 60hz. */
	inline constexpr u32 COMMAND_HISTORY = 128;

	/** The most commands one packet carries: every unacknowledged one, up to this many of the newest. */
	inline constexpr u32 MAX_COMMANDS_PER_PACKET = 32;

	/**
	 * The game's command type as the stream sees it: bytes, how they go on the wire, and what to do when
	 * one is missing. command_codec<T>() builds one from T's serialize(), so the stream itself never names
	 * a gameplay type.
	 */
	struct CommandCodec
	{
		u32 size		 = 0;		// sizeof the command: trivially copyable, at most MAX_COMMAND_BYTES
		const void* type = nullptr; // which type it was made for, so typed calls can check what they are given

		/** A default constructed command: what a tick runs on before any command has arrived. */
		std::array<u8, MAX_COMMAND_BYTES> empty = {};

		/** One command, for each kind of stream. previous is the command before it in the packet, or null. */
		bool (*write)(serialize::WriteStream& stream, const void* command, const void* previous) noexcept	  = nullptr;
		bool (*measure)(serialize::MeasureStream& stream, const void* command, const void* previous) noexcept = nullptr;
		bool (*read)(serialize::ReadStream& stream, void* command, const void* previous) noexcept			  = nullptr;

		/**
		 * Turns the last command into the stand-in for a tick whose own did not arrive in time: held input
		 * carries on, one-shot presses must not fire twice. Null repeats it as is.
		 */
		void (*repeat)(void* command) noexcept = nullptr;

		/**
		 * Folds a command that arrived after its tick ran into the command of the tick about to run,
		 * so a press the stand-in missed still happens, a tick late. Null drops late commands.
		 */
		void (*merge_late)(void* next, const void* late) noexcept = nullptr;
	};

	namespace detail
	{
		/** One address per command type: what CommandCodec::type points at. */
		template <class T> inline constexpr char COMMAND_TYPE = 0;

		/** A command copied out of the stream's storage, which is bytes, into a value of its own type. */
		template <class T> [[nodiscard]] T load_command(const void* bytes) noexcept
		{
			T value{};
			std::memcpy(&value, bytes, sizeof(T));
			return value;
		}

		/** One command on the wire: a bit saying it repeats the command before it, or the command itself. */
		template <class T, class Stream> bool serialize_command(Stream& stream, T& command, const T* previous) noexcept
		{
			bool same = false;
			if (Stream::IsWriting)
				same = previous != nullptr && command == *previous;

			serialize_bool(stream, same);

			if (!same)
				return command.serialize(stream);

			if (Stream::IsReading)
			{
				if (previous == nullptr)
					return serialize::serialize_fail(stream); // nothing to repeat: no writer sends this

				command = *previous;
			}

			return true;
		}

		template <class T, class Stream>
		bool write_command(Stream& stream, const void* command, const void* previous) noexcept
		{
			T value				= load_command<T>(command);
			const T before		= previous != nullptr ? load_command<T>(previous) : T{};
			const T* before_ptr = previous != nullptr ? &before : nullptr;
			return serialize_command(stream, value, before_ptr);
		}

		template <class T>
		bool read_command(serialize::ReadStream& stream, void* command, const void* previous) noexcept
		{
			T value				= {};
			const T before		= previous != nullptr ? load_command<T>(previous) : T{};
			const T* before_ptr = previous != nullptr ? &before : nullptr;

			if (!serialize_command(stream, value, before_ptr))
				return false;

			std::memcpy(command, &value, sizeof(T));
			return true;
		}
	}

	/**
	 * A codec for T from its serialize(), the one the rest of the wire uses. A command that equals the
	 * one before it consts one bit, which held input mostly does, so a packet's redundant copies are nearly
	 * free. Overloads found by argument dependent lookup fill the optional hooks:
	 *
	 * 		void repeat_command(T& command); 					// a stand-in from the last command
	 *		void merge_late_command(T& next, const T& late); 	// keep a late press
	 *
	 * The stream keeps commands as bytes and hands them to these as values of T, so T needs no particular
	 * alignment.
	 */
	template <class T> [[nodiscard]] CommandCodec command_codec() noexcept
	{
		static_assert(std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>,
					  "commands are plain values: copied as bytes, default constructed when none has arrived");
		static_assert(sizeof(T) <= MAX_COMMAND_BYTES, "commands are small: one tick of input");
		static_assert(std::equality_comparable<T>, "the codec sports repeated commands by comparing them");

		CommandCodec codec;
		codec.size = sizeof(T);
		codec.type = &detail::COMMAND_TYPE<T>;

		const T empty{};
		std::memcpy(codec.empty.data(), &empty, sizeof(T));

		codec.write = [](serialize::WriteStream& stream, const void* command, const void* previous) noexcept
		{ return detail::write_command<T>(stream, command, previous); };

		codec.measure = [](serialize::MeasureStream& stream, const void* command, const void* previous) noexcept
		{ return detail::write_command<T>(stream, command, previous); };

		codec.read = [](serialize::ReadStream& stream, void* command, const void* previous) noexcept
		{ return detail::read_command<T>(stream, command, previous); };

		if constexpr (requires(T& command) { repeat_command(command); })
		{
			codec.repeat = [](void* command) noexcept
			{
				T value = detail::load_command<T>(command);
				repeat_command(value);
				std::memcpy(command, &value, sizeof(T));
			};
		}

		if constexpr (requires(T& next, const T& late) { merge_late_command(next, late); })
		{
			codec.merge_late = [](void* next, const void* late) noexcept
			{
				T value = detail::load_command<T>(next);
				merge_late_command(value, detail::load_command<T>(late));
				std::memcpy(next, &value, sizeof(T));
			};
		}

		return codec;
	}

	/**
	 * How early the client's commands reach the server, in ticks, as the server measures it:
	 * positive means a command waited that long before its tick ran, negative that it came after
	 * and the tick ran on a stand-in. The server reports it in every packet; the client steers its
	 * clock by it.
	 */
	struct CommandTiming
	{
		u8 epoch	  = 0;	  // the client clock this describes; bumped every jump
		f32 mean	  = 0.0f; // smothed over roughly the last half second of commands
		f32 deviation = 0.0f; // mean absolute deviation, smoothed over roughly a second
	};

	/**
	 * The client half of the command stream. The client simulates every tick ahead of the server and
	 * records the command that drove it. Every packet carries all the commands the server has not yet
	 * acknowledged, oldest first, so a lost packet cost nothing as long as a later one arrives in time.
	 *
	 * Each command also carries how far behind its tick the world the player was looking at was drawn.
	 * The server needs that to rewind what the player aimed at (lag compensation).
	 */
	class CommandSender final
	{
	public:
		explicit CommandSender(const CommandCodec& codec) noexcept;

		/** Forgets every command; a new session starts here. */
		void reset() noexcept;

		/**
		 * Records the command for tick, which comes after every tick recorded before. view_delay is
		 * how many ticks behind the remote entities on screen wre, fractions included.
		 */
		template <class T> void push(Tick tick, const T& command, f32 view_delay) noexcept
		{
			EMBER_ASSERT(m_codec.type == &detail::COMMAND_TYPE<T> && "the codec was made for another type");
			push_bytes(tick, &command, view_delay);
		}

		/**
		 * Writes the command sectoin of the packet with this sequence: the clock epoch, then every command
		 * the server has not acknowledged, MAX_COMMANDS_PER_PACKET of the newest at most. The section never
		 * takes more than the rest of the packet, less reserve_bits for whatever the caller writes after it:
		 * if every command would not fit, the oldest are left out. The stream is a packet_writer() stream.
		 */
		void write(serialize::WriteStream& stream, Sequence packet, u8 epoch, u32 reserve_bites = 0) noexcept;

		/** A delivered packet acknowledges every command it carried, and every older one. */
		void on_notice(const PacketNotice& notice) noexcept;

		/**
		 * Copies the command recorded for tick into out; false if there is none. After the clock jumps back,
		 * the client replays these for ticks it has already sent: the server kees the first command it got
		 * for a tick.
		 */
		template <class T> [[nodiscard]] bool find(Tick tick, T& out) const noexcept
		{
			EMBER_ASSERT(m_codec.type == &detail::COMMAND_TYPE<T> && "the codec was made for another type");
			return find_bytes(tick, &out);
		}

		[[nodiscard]] Tick newest() const noexcept { return m_newest; }
		[[nodiscard]] Tick acknowledged() const noexcept { return m_acknowledged; }

	private:
		struct Record
		{
			u16 view								= 0; // sixteenths of a tick
			std::array<u8, MAX_COMMAND_BYTES> bytes = {};
		};

		struct Sent
		{
			Sequence packet = 0;
			bool valid		= false;
			Tick newest		= NO_TICK; // the newest command the packet carried
		};

		void push_bytes(Tick tick, const void* command, f32 view_delay) noexcept;
		[[nodiscard]] bool find_bytes(Tick tick, void* out) const noexcept;

		CommandCodec m_codec;
		TickRing<Record, COMMAND_HISTORY> m_commands;
		std::array<Sent, Connection::WINDOW> m_sent = {};
		Tick m_newest								= NO_TICK;
		Tick m_acknowledged							= NO_TICK;
	};

	/** Where the command a servertick ran on came from. */
	enum class CommandSource : u8
	{
		Received, // the client's own, in time
		Repeated, // a stand-in made from the last command: the client's own was lost or late
		Empty,	  // a default command: nothing has arrived yet
		Count
	};

	struct TakenCommand
	{
		CommandSource source = CommandSource::Empty;
		f32 view_delay		 = 0.0f; // ticks behind the command's tick that the player's screen showed
	};

	struct CommandQueueDef
	{
		f64 tick_seconds		= 1.0 / 60;
		f32 mean_smoothing		= 1.0f / 32.0f; // per command: about half a second at 60hz
		f32 deviation_smoothing = 1.0f / 64.0f; // per command: about a second
	};

	/**
	 * The server half: a small buffer of commands that arrived ahead of their tick, handed out one tick
	 * at a time, and the measurement of how early they arrive, which goes back to the client in every
	 * packet so it can run just far enough ahead.
	 *
	 * A tick whose command has not arrived on a stand-in made from the last command. If the real one
	 * turns up later, its presses are folded into the next tick so none is lost, and its lateness is
	 * measured: that is what tells the client to speed up.
	 */
	class CommandQueue final
	{
	public:
		explicit CommandQueue(const CommandCodec& codec, const CommandQueueDef& def = {}) noexcept;

		/** Forgets every command and measurement; a new session starts here. */
		void reset() noexcept;

		/**
		 * Reads a command section that arrived at `arrival`. False when it is malformed, and then
		 * none of its commands are kept: a sectoin count swhole or not at all.
		 */
		[[nodiscard]] bool read(serialize::ReadStream& stream, f64 arrival) noexcept;

		/**
		 * The command for tick, written to out. Ticks are taken in order, each once, at the moment
		 * the server simulates them.
		 */
		template <class T> TakenCommand take(Tick tick, T& out, f64 now) noexcept
		{
			EMBER_ASSERT(m_codec.type == &detail::COMMAND_TYPE<T> && "the codec was made for another type");
			return take_bytes(tick, &out, now);
		}

		/** The latest measurement of how early commands arrive. */
		[[nodiscard]] const CommandTiming& timing() const noexcept { return m_timing; }

		/** True once a command has been measured in the current epoch, so timing() means someting. */
		[[nodiscard]] bool has_timing() const noexcept { return m_samples > 0; }

		/** The newest command tick that has arrived. */
		[[nodiscard]] Tick newest() const noexcept { return m_newest; }

		/** Ticks that ran without their own command since the session began. */
		[[nodiscard]] u64 misses() const noexcept { return m_misses; }

	private:
		struct Slot
		{
			f64 arrival								= 0.0; // when the command arrived; meaningful when has_command
			f64 taken								= 0.0; // when its tick ran; meaningful once the tick has run
			u16 view								= 0;
			u8 epoch								= 0;	 // the client clock the command was sent on
			bool has_command						= false; // it arrived (in time, or late if taken_without)
			bool taken_without						= false; // the tick ran on a stand-in
			std::array<u8, MAX_COMMAND_BYTES> bytes = {};
		};

		TakenCommand take_bytes(Tick tick, void* out, f64 now) noexcept;
		void store(Tick tick, const u8* bytes, u16 view, u8 epoch, f64 arrival) noexcept;
		void sample(f32 slack) noexcept;

		CommandCodec m_codec;
		CommandQueueDef m_def;
		TickRing<Slot, COMMAND_HISTORY> m_slots;

		Tick m_last_taken = NO_TICK;
		Tick m_newest	  = NO_TICK;

		std::array<u8, MAX_COMMAND_BYTES> m_last  = {}; // the last command handed out
		std::array<u8, MAX_COMMAND_BYTES> m_carry = {}; // late presses waiting for the next tick
		u16 m_last_view							  = 0;
		bool m_has_last							  = false;
		bool m_has_carry						  = false;
		bool m_has_epoch						  = false; // an epoch has been seen, so older ones can be told apart

		CommandTiming m_timing;
		u64 m_samples = 0;
		u64 m_misses  = 0;
	};

	/** Writes the server's report on command timing into a packet to the client: 29 bits at most. */
	void write_command_timing(serialize::WriteStream& stream, const CommandQueue& queue) noexcept;

	/** Reads it back; out is set when the packet carried a report. False when malformed. */
	[[nodiscard]] bool read_command_timing(serialize::ReadStream& stream, CommandTiming& out, bool& present) noexcept;
}

namespace ember
{
	EMBER_ENUM_NAMES(net::CommandSource, "Received", "Repeated", "Empty");
}
