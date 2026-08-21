#pragma once

#include <ember/core/common.h>

namespace ember
{
	/**
	 * Clear value for a color attachment: 16 bytes reinterpreted by the attachment's format.
	 * Float formats read r/g/b/a directly; integer formats need the bit patterns produced by
	 * from_uint()/from_sint(). Kept as four floats (not a union) so designated initializers
	 * stay trivial: `.clear = {0.1f, 0.2f, 0.3f, 1.0f}`.
	 */
	struct Color
	{
		f32 r = 0.0f;
		f32 g = 0.0f;
		f32 b = 0.0f;
		f32 a = 1.0f;

		[[nodiscard]] static constexpr Color from_uint(u32 r, u32 g, u32 b, u32 a) noexcept
		{
			return {std::bit_cast<f32>(r), std::bit_cast<f32>(g), std::bit_cast<f32>(b), std::bit_cast<f32>(a)};
		}

		[[nodiscard]] static constexpr Color from_sint(i32 r, i32 g, i32 b, i32 a) noexcept
		{
			return {std::bit_cast<f32>(r), std::bit_cast<f32>(g), std::bit_cast<f32>(b), std::bit_cast<f32>(a)};
		}
	};

	static_assert(sizeof(Color) == 16, "ClearColor must be layout-compatible with VkClearColorValue");
}
