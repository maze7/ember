#pragma once

#include <cstdint>

namespace ember
{
	// Unsigned integers
	using u8 = uint8_t;
	using u16 = uint16_t;
	using u32 = uint32_t;
	using u64 = uint64_t;

	// Signed integers
	using i8 = int8_t;
	using i16 = int16_t;
	using i32 = int32_t;
	using i64 = int64_t;

	// Sized floating points
	using f32 = float;
	using f64 = double;

	consteval u64 operator""_kb(unsigned long long v) { return static_cast<u64>(v) * 1024; }
	consteval u64 operator""_mb(unsigned long long v) { return static_cast<u64>(v) * 1024 * 1024; }
	consteval u64 operator""_gb(unsigned long long v) { return static_cast<u64>(v) * 1024 * 1024 * 1024; }
}
