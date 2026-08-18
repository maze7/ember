#pragma once

#include <ember/core/common.h>

#include <algorithm>

namespace ember
{

	/// Remaps a value from [from_min, from_max] to [0, 1], clamped.
	[[nodiscard]] constexpr f32 clamped_map(f32 value, f32 from_min, f32 from_max) noexcept
	{
		return std::clamp((value - from_min) / (from_max - from_min), 0.0f, 1.0f);
	}

	/// Returns the sign of the given value.
	[[nodiscard]] constexpr i32 sign_of(f32 value) noexcept
	{
		return static_cast<i32>(value > 0.0f) - static_cast<i32>(value < 0.0f);
	}
}
