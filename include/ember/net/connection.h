#pragma once

#include <ember/containers/ring_buffer.h>
#include <ember/net/sequence.h>
#include <ember/net/serialize.h>

#include <array>
#include <limits>

namespace ember::net
{
	struct ConnectionDef
	{
		f64 timeout = 10.0; // seconds without an applied packet before the connection counts as gone
	};

	/** What became of one packet this end sent. Every sent sequence gets exactly once, oldest first. */
	struct PacketNotice
	{
		Sequence sequence = 0;
		bool delivered	  = false;
	};

	/** One end's view of the path, measured from the acks the other end returns. */
	struct ConnectionStats
	{
		f64 rtt			 = 0.0;	 // smoothed round trip in seconds, RFC 6298; 0 until the first sample
		f64 rtt_variance = 0.0;	 // smoothed mean deviation of the round trip
		f64 rtt_min		 = 0.0;	 // lowest sample in the last ten seconds: the path without queueing
		f64 rtt_latest	 = 0.0;	 // the newest sample
		f32 loss		 = 0.0f; // fraction of sent packets lost, smoothed over roughly the last hundred
		f32 receive_loss = 0.0f; // fraction of the peer's packets never applied here, smoothed the same way
		f32 send_rate	 = 0.0f; // bytes per second, updated as packets flow
		f32 receive_rate = 0.0f;

		u64 packets_sent	  = 0;
		u64 packets_received  = 0; // applied, so acked
		u64 packets_delivered = 0;
		u64 packets_lost	  = 0;
		u64 packets_skipped	  = 0; // the peer's, never applied: lost on the way, or overtaken and then stale
		u64 packets_stale	  = 0; // late or repeated arrivals, dropped unread
		u64 packets_malformed = 0; // headers that did not decode
		u64 bytes_sent		  = 0;
		u64 bytes_received	  = 0; // in applied packets
	};

	/**
	 * The protocol's ground floor for one peer: numbers the packets this end sends, acks the ones it receives,
	 * and tells the layers above which of their packets arrived. It carries no payload of its own; a packet is
	 * this header and whatever the layers above write after it, all of it travelling as one unreliable message
	 * of the transport.
	 *
	 *   sequence   16  this packet's number
	 *   has_ack     1  clear until the first packet from the peer has been applied
	 *   ack        16  the newest sequence applied from the peer
	 *   ack_bits   32  bit i set: ack - 1 - i was applied too
	 *   ack_delay   8  how long `ack` waited here before this packet left, in half milliseconds
	 *
	 * Every packet acks the last 33, so an ack only goes missing if 33 packets in a row do.
	 * Delivery is in order or not at all: a packet older than one already applied is dropped. That costs the
	 * rare reordered packet and buys an exact answer the moment an ack arrives: anything older than the acked
	 * packet and not acked with it will never be applied, so it is lost. Each sent packet gets one notice,
	 * delivered or lost, and the layers above keep their own record of what each packet carried (a manifest) to
	 * act on it: resend a lost message, re-mark lost state sirty, retire acked commands. Nothing waits on a
	 * resend timer. This is the Tribes "notify" protocol.
	 *
	 * The round trip comes from the same acks: send time to ack arrival, minus the ack_delay the peer reports,
	 * so a peer that sends only every other tick does not inflate it.
	 *
	 * Single threaded: the endpoint that owns it drives it.
	 */
	class Connection final
	{
	public:
		/** Sent packets remembered while their fate is unknown. */
		static constexpr u32 WINDOW = 256;

		/** The header's size when it carries acks, which is every packet after the first: for packet budgets. */
		static constexpr u32 HEADER_BITS = 16 + 1 + 16 + 32 + 8;

		enum class Arrival : u8
		{
			Fresh,	   // newer than anything applied: read the payload, then acknowledge()
			Stale,	   // a repeat, or overtaken by a newer packet: drop it unread
			Malformed, // the header does not decode: drop it
		};

		explicit Connection(const ConnectionDef& def = {}, f64 now = 0.0) noexcept;

		/** Forgets the peer: numbering, acks, notices and statistics start over. */
		void reset(f64 now) noexcept;

		/**
		 * Writes the next packet's header and returns its sequence. The packet counts as sent from here and will
		 * get exactly one notice; write its payload next and hand the bytes to the transport. If the window is
		 * full, its oldest packet is given up as lost first.
		 */
		[[nodiscard]] Sequence write_header(serialize::WriteStream& stream, f64 now) noexcept;

		/** Counts a finished packet's bytes toward the send rate. */
		void count_sent(u32 bytes, f64 now) noexcept;

		/**
		 * Reads a packet's header and leaves the stream at its payload. The acks it carries are taken in whatever
		 * the verdict, as notices, unless the header is Malformed.
		 */
		[[nodiscard]] Arrival read_header(serialize::ReadStream& stream, Sequence& sequence, f64 arrival) noexcept;

		/**
		 * Applies a Fresh packet once its payload has been read and acted on: the peer will see it acked, and
		 * anything older that arrives late is Stale. A packet left unacknowledged, because its payload failed
		 * to decode, is lost as far as the sender can tell, so it sends again whatever mattered.
		 */
		void acknowledge(Sequence sequence, u32 bytes, f64 arrival) noexcept;

		/** The next delivery notice, oldest first; false when there are none. */
		[[nodiscard]] bool take_notice(PacketNotice& notice) noexcept;

		/** True once nothing has been applied from the peer for longer than the timeout. */
		[[nodiscard]] bool timed_out(f64 now) const noexcept;

		[[nodiscard]] const ConnectionStats& stats() const noexcept { return m_stats; }

		/** The sequence the next packet will carry. */
		[[nodiscard]] Sequence next_sequence() const noexcept { return m_next_sequence; }

		/** Packets sent whose fate is not known yet. */
		[[nodiscard]] u32 in_flight() const noexcept { return static_cast<u16>(m_next_sequence - m_oldest_pending); }

	private:
		struct RateMeter
		{
			f64 start = 0.0;
			u64 bytes = 0;

			void add(u32 count, f64 now, f32& rate) noexcept;
		};

		struct MinBucket
		{
			i64 second = std::numeric_limits<i64>::min();
			f64 min	   = 0.0;
		};

		void take_acks(Sequence ack, u32 ack_bits, u32 ack_delay, f64 arrival) noexcept;
		void settle_oldest(bool delivered) noexcept;
		void sample_rtt(f64 rtt, f64 now) noexcept;

		ConnectionDef m_def;
		ConnectionStats m_stats;

		// Sending. Every sequence in [m_oldest_pending, m_next_sequence) awaits its notice.
		std::array<f64, WINDOW> m_send_times = {};
		Sequence m_next_sequence			 = 0;
		Sequence m_oldest_pending			 = 0;

		// Receiving.
		bool m_has_received = false;
		Sequence m_received = 0;   // the newest sequence applied
		u32 m_received_bits = 0;   // bit i: m_received - 1 - i applied too
		f64 m_received_time = 0.0; // when m_received arrived, for ack_delay
		f64 m_last_heard	= 0.0; // when the newest applied packet arrived, for the timeout

		RingBuffer<PacketNotice, 2 * WINDOW> m_notices;

		std::array<MinBucket, 10> m_rtt_window = {};
		bool m_has_rtt						   = false;
		RateMeter m_send_meter;
		RateMeter m_receive_meter;
	};
}
