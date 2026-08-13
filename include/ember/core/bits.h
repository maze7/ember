#pragma once

#include <ember/core/common.h>

namespace ember
{
	// Low level integer helpers used by memory, hashing and platform code. These functions are
	// constexpr/noexcept so they can be used in compile-time layout checks and in hot runtime paths.

	template <class T>
	concept UnsignedInteger = std::is_integral_v<T> && std::is_unsigned_v<T> && !std::is_same_v<T, bool>;

	template <UnsignedInteger T>
	[[nodiscard]] EMBER_FINLINE constexpr bool is_power_of_two(T value) noexcept
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	template <UnsignedInteger T>
	[[nodiscard]] EMBER_FINLINE constexpr T align_down(T value, T alignment) noexcept
	{
		// Assert instead of silently accepting bad input. Bad alignment usually indicates a caller
		// contract bug and should be caught early.
		EMBER_ASSERT(is_power_of_two(alignment));
		return value & ~(alignment - 1);
	}

	template <UnsignedInteger T>
	[[nodiscard]] EMBER_FINLINE constexpr T align_up(T value, T alignment) noexcept
	{
		// Assert instead of silently accepting bad input. Bad alignment usually indicates a caller
		// contract bug and should be caught early.
		EMBER_ASSERT(is_power_of_two(alignment));
		return (value + (alignment - 1)) & ~(alignment - 1);
	}

	template <typename T>
	[[nodiscard]] EMBER_FINLINE constexpr T* align_up(T* ptr, size_t alignment) noexcept
	{
		const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
		const T mask = alignment - 1;

		EMBER_ASSERT(is_power_of_two(alignment));
		EMBER_ASSERT(value <= (std::numeric_limits<T>::max() - mask));

		return reinterpret_cast<T*>((value + mask) & ~mask);
	}

	template <typename T>
	[[nodiscard]] EMBER_FINLINE constexpr T* align_down(T* ptr, size_t alignment) noexcept
	{
		const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);

		EMBER_ASSERT(is_power_of_two(alignment));
		return reinterpret_cast<T*>(value & ~(alignment - 1));
	}

	[[nodiscard]] EMBER_FINLINE constexpr bool is_aligned(const void* ptr, size_t alignment) noexcept
	{
		if (!is_power_of_two(alignment))
			return false;

		return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
	}

	template <UnsignedInteger T>
	[[nodiscard]] EMBER_FINLINE constexpr bool checked_add(T a, T b, T& out) noexcept
	{
		if (a > std::numeric_limits<T>::max() - b)
			return false;

		out = a + b;
		return true;
	}

	template <UnsignedInteger T>
	[[nodiscard]] EMBER_FINLINE constexpr bool checked_sub(T a, T b, T& out) noexcept
	{
		if (a < b)
			return false;

		out = a - b;
		return true;
	}

	template <UnsignedInteger T>
	[[nodiscard]] EMBER_FINLINE constexpr bool checked_mul(T a, T b, T& out) noexcept
	{
		if (a != 0 && b > std::numeric_limits<T>::max() / a)
			return false;

		out = a * b;
		return true;
	}
}
