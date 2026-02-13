#pragma once

#include "SDL3/SDL_iostream.h"
#include "logger.h"
#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// unsigned integers
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// signed integers
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

// sized floats
using f32 = float;
using f64 = double;

namespace Ember
{
	template <typename T>
	struct EnumNames;

	template <typename T>
	using Optional = std::optional<T>;
	inline constexpr auto NullOpt = std::nullopt;

	template <typename T>
	using Ref = std::shared_ptr<T>;

	template <typename T>
	using WeakRef = std::weak_ptr<T>;

	template <typename T>
	using Unique = std::unique_ptr<T>;

	template <typename T, typename... Args>
	Ref<T> make_ref(Args&&... args) {
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	template <typename T, typename... Args>
	Unique<T> make_unique(Args&&... args) {
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	// TODO: Move this to a utility method
	inline std::vector<u8> load_file(const char* path) {
		size_t code_size;
		void* code = SDL_LoadFile(path, &code_size);

		if(code != nullptr) {
			std::vector<u8> contents((u8*)code, (u8*)code + code_size);
			SDL_free(code);
			return contents;
		}

		return {};
	}

	using Exception = std::runtime_error;
}

#if !defined(_MSC_VER)
#include <signal.h>
#endif

#if defined(_MSC_VER)
#define EMBER_INLINE inline
#define EMBER_FINLINE __forceinline
#define EMBER_DEBUG_BREAK() __debugbreak();
#define EMBER_DISABLE_WARNING(warning_number) __pragma(warning(disable : warning_number))
#else
#define EMBER_INLINE inline
#define EMBER_FINLINE always_inline
#define EMBER_DEBUG_BREAK() raise(SIGTRAP);
#define EMBER_DISABLE_WARNING(warning_number)
#endif

#define EMBER_STR(L) #L
#define EMBER_MAKESTR(L) EMBER_STR(L)

#if !defined(NDEBUG) || defined(EMBER_PROFILE)
#define EMBER_ASSERT(condition)                                                                                        \
	if(!(condition)) {                                                                                                 \
		Ember::Log::error(__FILE__ "(" EMBER_MAKESTR(__LINE__) "): ASSERT");                                           \
		EMBER_DEBUG_BREAK();                                                                                           \
	}
#else
#define EMBER_ASSERT(condition) (condition) // asserts are stripped for release builds
#endif


#define EMBER_ENUM_NAMES(Type, ...)                                   													\
template <>                                                     														\
struct EnumNames<Type> {                                        														\
    constexpr auto operator()() const {                         														\
        return std::to_array<const char*>({__VA_ARGS__});       														\
    }                                                           														\
};
