#pragma once

#include <ember/core/common.h>

namespace ember
{
	struct RepeatConfig
	{
		u64 delay_ns	= 400'000'000ull;
		u64 interval_ns = 30'000'000ull;
	};

	namespace input_detail
	{
		[[nodiscard]] constexpr bool repeated(
			bool pressed,
			bool down,
			u64 pressed_at,
			u64 previous_frame,
			u64 current_frame,
			RepeatConfig config) noexcept
		{
			if (pressed)
				return true;

			if (!down || config.interval_ns == 0 || current_frame < pressed_at)
				return false;

			u64 held_now = current_frame - pressed_at;
			if (held_now < config.delay_ns)
				return false;

			u64 held_before = previous_frame > pressed_at ? previous_frame - pressed_at : 0;

			// First frame that crosses the initial repeat delay.
			if (held_before < config.delay_ns)
				return true;

			u64 previous_interval = (held_before - config.delay_ns) / config.interval_ns;
			u64 current_interval  = (held_now - config.delay_ns) / config.interval_ns;

			return previous_interval != current_interval;
		}
	}
}
