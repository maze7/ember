#pragma once

#include <ember/core/common.h>
#include <ember/core/hash.h>

/**
 * The names audio goes by. Sounds are made in FMOD Studio and found by the paths it gives them; code
 * and data name them by those paths and nothing else. A path written in code is hashed where it is
 * written, by the compiler, so playing a sound never touches a string.
 */
namespace ember::audio
{
	/** An event or a snapshot by its Studio path: "event:/Footstep", "snapshot:/Paused". */
	struct Event
	{
		u64 id			 = 0;
		const char* path = nullptr; // the literal it came from, for messages; null for one read from a file

		constexpr Event() noexcept = default;

		/** From a literal, hashed at compile time. */
		template <size_t N>
		consteval Event(const char (&literal)[N]) noexcept : id(hash_text(StringView(literal, N - 1))), path(literal)
		{
		}

		/** From a path read while running: a table's, a tool's. */
		[[nodiscard]] static constexpr Event from(StringView path) noexcept
		{
			Event event;
			event.id = path.empty() ? 0 : hash_text(path);
			return event;
		}

		/** From the hash itself, as an animation's pose carries it. */
		[[nodiscard]] static constexpr Event of(u64 id) noexcept
		{
			Event event;
			event.id = id;
			return event;
		}

		[[nodiscard]] constexpr explicit operator bool() const noexcept { return id != 0; }
		[[nodiscard]] friend constexpr bool operator==(Event a, Event b) noexcept { return a.id == b.id; }
	};

	/** A parameter by the name Studio gives it: "Surface". */
	struct Param
	{
		u64 id = 0;

		constexpr Param() noexcept = default;

		template <size_t N>
		consteval Param(const char (&literal)[N]) noexcept : id(hash_text(StringView(literal, N - 1)))
		{
		}

		[[nodiscard]] static constexpr Param from(StringView name) noexcept
		{
			Param param;
			param.id = name.empty() ? 0 : hash_text(name);
			return param;
		}

		[[nodiscard]] friend constexpr bool operator==(Param a, Param b) noexcept { return a.id == b.id; }
	};

	/** A parameter and the value to give it. */
	struct ParamValue
	{
		Param param;
		f32 value = 0.0f;
	};
}
