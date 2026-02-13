#pragma once

#include "core/common.h"
#include "glm.hpp"

namespace Ember
{
	static constexpr double EPSILON = 0.000001;
	static constexpr double PI = std::numbers::pi;
	static constexpr double TWO_PI = 2 * std::numbers::pi;

	// @brief Checks if any bits from 'check' are present in 'flags'.
	template <class T> requires std::is_integral_v<std::underlying_type_t<T>>
	inline constexpr bool bitmask_has(T flags, T check) {
		using UT = std::underlying_type_t<T>;
		return (static_cast<UT>(flags) & static_cast<UT>(check)) != 0;
	}

	// @brief Checks if ALL bits from 'check' are present in 'flags'.
	template <class T> requires std::is_integral_v<std::underlying_type_t<T>>
	inline constexpr bool bitmask_has_all(T flags, T check) {
		using UT = std::underlying_type_t<T>;
		return (static_cast<UT>(flags) & static_cast<UT>(check)) == static_cast<UT>(check);
	}

	// @brief Correctly adds bits to the flags.
	template <class T> requires std::is_integral_v<std::underlying_type_t<T>>
	inline constexpr T bitmask_with(T flags, T with) {
		using UT = std::underlying_type_t<T>;
		return static_cast<T>(static_cast<UT>(flags) | static_cast<UT>(with));
	}

	// @brief Correctly removes bits from the flags.
	template <class T> requires std::is_integral_v<std::underlying_type_t<T>>
	inline constexpr T bitmask_without(T flags, T without) {
		using UT = std::underlying_type_t<T>;
		return static_cast<T>(static_cast<UT>(flags) & ~static_cast<UT>(without));
	}

	template <class T>
	constexpr T abs(T x) {
		return x < 0 ? -x : x;
	}

	template <class T>
	constexpr T sign(T x) {
		return static_cast<T>(x == 0 ? 0 : (x < 0 ? -1 : 1));
	}

	template <class T, class TMin, class TMax>
	constexpr T clamp(T value, TMin min, TMax max) {
		return value < min ? static_cast<T>(min) : (value > max ? static_cast<T>(max) : value);
	}

	template <class T>
	constexpr T min(T a, T b) {
		return (T)(a < b ? a : b);
	}

	template <class T, typename... Args>
	constexpr T min(const T& a, const T& b, const Args&... args) {
		return min(a, min(b, args...));
	}

	template <class T>
	constexpr T max(T a, T b) {
		return (T)(a > b ? a : b);
	}

	template <class T, typename... Args>
	constexpr T max(const T& a, const T& b, const Args&... args) {
		return max(a, max(b, args...));
	}

	constexpr f32 approach(f32 t, f32 target, f32 delta) {
		return t < target ? min(t + delta, target) : max(t - delta, target);
	}

	constexpr f32 map(f32 t, f32 old_min, f32 old_max, f32 new_min, f32 new_max) {
		return new_min + ((t - old_min) / (old_max - old_min)) * (new_max - new_min);
	}

	constexpr f32 clamped_map(f32 val, const f32 min, f32 max, f32 new_min = 0, f32 new_max = 1) {
		return clamp((val - min) / (max - min), 0, 1) * (new_max - new_min) + new_min;
	}

	constexpr f32 lerp(f32 a, f32 b, f32 t) {
		return a + (b - a) * t;
	}

	inline glm::vec2 grid_align(glm::vec2 val, float grid_size = 16) {
		return floor(val / grid_size) * grid_size;
	}

	inline glm::vec2 screen_to_world(const glm::vec2& screen_pos, const glm::vec2& screen_size, const glm::mat4 inv_view_proj) {
		glm::vec2 ndc = (screen_pos / screen_size) * 2.0f - 1.0f;
		glm::vec4 world = inv_view_proj * glm::vec4(ndc, 0.0f, 1.0f);
		return glm::vec2(world);
	}

	inline glm::vec2 world_to_screen(const glm::vec2& world_pos, const glm::vec2& screen_size, const glm::mat4& view_projection) {
		glm::vec4 clip = view_projection * glm::vec4(world_pos, 0.0f, 1.0f);
		glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w; // perspective divide (w=1 for ortho)
		return (ndc + 1.0f) * 0.5f * screen_size;
	}
}
