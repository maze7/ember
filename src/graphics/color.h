#pragma once

#include "core/common.h"
#include "core/hash.h"
#include "math/math.h"

namespace Ember
{
	struct Color
	{
		u8 r = 0;
		u8 g = 0;
		u8 b = 0;
		u8 a = 0;

		// Predefined Colours
		static const Color Transparent;
		static const Color White;
		static const Color Black;
		static const Color LightGray;
		static const Color Gray;
		static const Color DarkGray;
		static const Color Red;
		static const Color Green;
		static const Color Blue;
		static const Color Yellow;
		static const Color Aqua;
		static const Color Cyan;
		static const Color Magenta;
		static const Color CornflowerBlue;
		static const Color Purple;

		// Constructors
		constexpr Color() = default;
		constexpr Color(u32 rgba)
			: r((rgba >> 24) & 0xFF), g((rgba >> 16) & 0xFF), b((rgba >> 8) & 0xFF), a(rgba & 0xFF) {}

		constexpr Color(u8 r, u8 g, u8 b, u8 a = 255)
			: r(r), g(g), b(b), a(a) {}

		constexpr Color(int r, int g, int b, int a = 255)
			: r((u8)r), g((u8)g), b((u8)b), a((u8)a) {}

		explicit constexpr Color(float r, float g, float b, float a = 1.0f)
			: r((u8)(r * 255)), g((u8)(g * 255)), b((u8)(b * 255)), a((u8)(a * 255)) {}

		[[nodiscard]] constexpr u32 rgba() const {
			return  ((u32)r << 24) | ((u32)g << 16) |
					((u32)b << 8) | (u32)a;
		}

		[[nodiscard]] constexpr u32 abgr() const {
			return  ((u32) a << 24) | ((u32) b << 16) |
					((u32) g << 8) | (u32) r;
		}

		[[nodiscard]] constexpr Color premultiply() const {
			float alpha = a / 255.0f;
			return Color(
				(u8)std::round(r * alpha),
				(u8)std::round(g * alpha),
				(u8)std::round(b * alpha),
				a
			);
		}

		auto operator<=>(const Color&) const = default;

		static Color lerp(const Color& a, const Color& b, float amount) {
			amount = clamp(amount, 0.0f, 1.0f);
			return Color(
				(u8)(a.r + (b.r - a.r) * amount),
				(u8)(a.g + (b.g - a.g) * amount),
				(u8)(a.b + (b.b - a.b) * amount),
				(u8)(a.a + (b.a - a.a) * amount));
		}
	};
}

namespace std
{
	template<>
    struct hash<Ember::Color>
    {
        // The return type must be size_t for std::hash
        size_t operator()(const Ember::Color& c) const noexcept {
            return Ember::combined_hash(c.r, c.g, c.b, c.a);
        }
    };
}
