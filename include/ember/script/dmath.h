#pragma once

#include <ember/core/common.h>

/**
 * Deterministic math for scripts: what the sim's `math` table calls instead of the platform's libm,
 * whose sin and exp differ in their last bits between C libraries. Every function here is adds,
 * multiplies, divides and square roots on doubles, which IEEE 754 fixes to the bit, and the module
 * is built without fused multiply-adds, so two machines agree exactly. Accuracy is a few units in
 * the last place: plenty for gameplay, and the same everywhere, which is the point.
 */
namespace ember::script::dmath
{
	[[nodiscard]] f64 sin(f64 x) noexcept;
	[[nodiscard]] f64 cos(f64 x) noexcept;
	[[nodiscard]] f64 tan(f64 x) noexcept;
	[[nodiscard]] f64 asin(f64 x) noexcept;
	[[nodiscard]] f64 acos(f64 x) noexcept;
	[[nodiscard]] f64 atan(f64 x) noexcept;
	[[nodiscard]] f64 atan2(f64 y, f64 x) noexcept;
	[[nodiscard]] f64 exp(f64 x) noexcept;
	[[nodiscard]] f64 log(f64 x) noexcept;
	[[nodiscard]] f64 pow(f64 x, f64 y) noexcept;
}
