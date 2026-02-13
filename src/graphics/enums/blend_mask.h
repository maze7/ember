#pragma once

#include "core/common.h"

namespace Ember
{
	enum class BlendMask : u32
	{
		None    = 0,
		Red     = 1,
		Green   = 2,
		Blue    = 4,
		Alpha   = 8,
		RGB     = Red | Green | Blue,
		RGBA    = Red | Green | Blue | Alpha
	};

	[[nodiscard]] constexpr BlendMask operator|(BlendMask lhs, BlendMask rhs) {
        using T = std::underlying_type_t<BlendMask>;
        return static_cast<BlendMask>(static_cast<T>(lhs) | static_cast<T>(rhs));
    }

    [[nodiscard]] constexpr BlendMask operator&(BlendMask lhs, BlendMask rhs) {
        using T = std::underlying_type_t<BlendMask>;
        return static_cast<BlendMask>(static_cast<T>(lhs) & static_cast<T>(rhs));
    }

    // You can also add |= and &= for convenience
    constexpr BlendMask& operator|=(BlendMask& lhs, BlendMask rhs) {
        lhs = lhs | rhs;
        return lhs;
    }
}
