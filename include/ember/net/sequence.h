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
}
