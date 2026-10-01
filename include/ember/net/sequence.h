#pragma once

#include <ember/core/common.h>

namespace ember::net
{
	/**
	 * Packet sequence numbers: 16 bits on the wirte, wrapping every 65536 packets (18 minutes at 60Hz).
	 * Compare them only through the helpers below, which take the shorter way around the circle as the
	 * truth (RFC 1982 serial number arithmetic).
	 */
	using Sequence = u16;

	/** How far ahead of b, negative when it is behind. Exact for distances under 32768. */
	constexpr i32 sequence_delta(Sequence a, Sequence b) noexcept { return static_cast<i16>(static_cast<u16>(a - b)); }

	/** True when a was sent after b. */
	constexpr bool sequence_newer(Sequence a, Sequence b) noexcept { return sequence_delta(a, b) > 0; }

	/**
	 * The largest packet or message the protocol sends. GameNetworkingSockets puts at most 1132
	 * bytes of one message in a single UDP packet at its default MTU (1300, less 52 of framing, 16
	 * of AES-GCM tag and 100 kept for its own headers); a larger unreliable message is fragmented,
	 * and losing any fragment loses all of it. 1024 stays clear of that with room for relay
	 * headers, and is a multiple of 8, as serialize's writer needs. Every transport carries this
	 * much in one piece.
	 */
	inline constexpr u32 MAX_PACKET_BYTES = 1024;
}
