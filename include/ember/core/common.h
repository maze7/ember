#pragma once

#include <ankerl/unordered_dense.h>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#if defined(_MSC_VER)
	#define EMBER_INLINE __inline
	#define EMBER_FINLINE __forceinline
	#define EMBER_DEBUG_BREAK() __debugbreak()
	#define EMBER_UNREACHABLE() __assume(false)
	#define EMBER_DISABLE_WARNING(n) __pragma(warning(disable : n))
#elif defined(__clang__)
	#define EMBER_INLINE inline
	#define EMBER_FINLINE inline __attribute__((always_inline))
	#define EMBER_DEBUG_BREAK() __builtin_debugtrap()
	#define EMBER_UNREACHABLE() __builtin_unreachable()
	#define EMBER_DISABLE_WARNING(n) // clang uses -Wno-xxx, not numeric
#elif defined(__GNUC__)
	#define EMBER_INLINE inline
	#define EMBER_FINLINE inline __attribute__((always_inline))
	#define EMBER_DEBUG_BREAK() __builtin_trap()
	#define EMBER_UNREACHABLE() __builtin_unreachable()
	#define EMBER_DISABLE_WARNING(n)
#else
	#error "Unsupported compiler"
#endif

#if !defined(NDEBUG) || defined(EMBER_PROFILE)
	#define EMBER_ASSERT(expr)                                                                                         \
		do                                                                                                             \
		{                                                                                                              \
			if (!(expr))                                                                                               \
			{                                                                                                          \
				if (ember::assert_fail(#expr, nullptr, __FILE__, __LINE__, __FUNCTION__))                              \
					EMBER_DEBUG_BREAK();                                                                               \
			}                                                                                                          \
		} while (0)

	#define EMBER_UNREACHABLE_ASSERT()                                                                                 \
		do                                                                                                             \
		{                                                                                                              \
			(void)ember::assert_fail("Unreachable code executed", nullptr, __FILE__, __LINE__, __FUNCTION__);          \
			EMBER_DEBUG_BREAK();                                                                                       \
		} while (0)
#else
	#define EMBER_ASSERT(expr) ((void)0)
	#define EMBER_UNREACHABLE_ASSERT() ((void)0)
#endif

#ifndef EMBER_CACHE_LINE
	#define EMBER_CACHE_LINE 64
#endif // EMBER_CACHE_LINE

static_assert((EMBER_CACHE_LINE & (EMBER_CACHE_LINE - 1)) == 0);

namespace ember
{
	// Unsigned integers
	using u8  = uint8_t;
	using u16 = uint16_t;
	using u32 = uint32_t;
	using u64 = uint64_t;

	// Signed integers
	using i8  = int8_t;
	using i16 = int16_t;
	using i32 = int32_t;
	using i64 = int64_t;

	// Sized floating points
	using f32 = float;
	using f64 = double;

	consteval u64 operator""_kb(unsigned long long v) { return static_cast<u64>(v) * 1024; }
	consteval u64 operator""_mb(unsigned long long v) { return static_cast<u64>(v) * 1024 * 1024; }
	consteval u64 operator""_gb(unsigned long long v) { return static_cast<u64>(v) * 1024 * 1024 * 1024; }

	// PMR containers
	template <typename T> using Vector = std::pmr::vector<T>;
	using String					   = std::pmr::string;
	using StringView				   = std::string_view;

	template <typename K, typename V, typename Hash = ankerl::unordered_dense::hash<K>>

	using HashMap = ankerl::unordered_dense::pmr::map<K, V, Hash>;
	template <typename K, typename Hash = ankerl::unordered_dense::hash<K>>
	using HashSet = ankerl::unordered_dense::pmr::set<K, Hash>;

	bool assert_fail(const char* condition, const char* message, const char* file, int line, const char* func);

	template <typename T> struct EnumNames;
	template <typename T> constexpr auto enum_names() { return EnumNames<T>{}(); }

	template <typename Type, size_t Count> constexpr void check_enum_count()
	{
		if constexpr (requires { Type::Count; })
		{
			static_assert(Count == static_cast<size_t>(Type::Count), "EMBER_ENUM_NAMES: name count mismatch!");
		}
	}

#define EMBER_ENUM_NAMES(Type, ...)                                                                                    \
	template <> struct EnumNames<Type>                                                                                 \
	{                                                                                                                  \
		constexpr auto operator()() const                                                                              \
		{                                                                                                              \
			constexpr const char* raw[] = {__VA_ARGS__};                                                               \
			constexpr auto arr			= std::to_array(raw);                                                          \
			check_enum_count<Type, arr.size()>();                                                                      \
			return arr;                                                                                                \
		}                                                                                                              \
	};
}
