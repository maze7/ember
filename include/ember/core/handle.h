#pragma once

#include <ember/core/common.h>

namespace ember
{
	template <typename T, typename C = u16> requires(std::unsigned_integral<C> && !std::same_as<C, bool>)
	struct Handle
	{
		static_assert(sizeof(C) <= sizeof(u32), "Handle supports up to 64 total bits");

		using tag_type = T;
		using component_type = C;
		using packed_type = std::conditional_t<sizeof(C) == 1, u16, std::conditional_t<sizeof(C) == 2, u32, u64>>;

		static constexpr u32 COMPONENT_BITS = std::numeric_limits<C>::digits;
		static constexpr u32 TOTAL_BITS = COMPONENT_BITS * 2;

		C index = 0;
		C generation = 0;

		[[nodiscard]] constexpr bool is_null() const noexcept { return generation == 0; }

		constexpr bool operator==(const Handle&) const noexcept = default;

		[[nodiscard]] constexpr packed_type to_bits() const noexcept
		{
			return (static_cast<packed_type>(generation) << COMPONENT_BITS) | static_cast<packed_type>(index);
		}

		[[nodiscard]] static constexpr Handle from_bits(packed_type bits) noexcept
		{
			constexpr packed_type mask = std::numeric_limits<C>::max();
			return Handle{static_cast<C>(bits & mask), static_cast<C>(bits >> COMPONENT_BITS)};
		}
	};

	template <typename T> using Handle32 = Handle<T, u16>;
	template <typename T> using Handle64 = Handle<T, u32>;
}

namespace std
{
	template <typename T, typename Component> struct hash<ember::Handle<T, Component>>
	{
		[[nodiscard]] constexpr size_t operator()(ember::Handle<T, Component> handle) const noexcept
		{
			ember::u64 value = static_cast<ember::u64>(handle.to_bits());

			// SplitMix64 finalizer. Good avalanche for sequential pool indices and
			// generations, while compiling to a short integer-only sequence.
			value ^= value >> 30;
			value *= 0xbf58476d1ce4e5b9ull;
			value ^= value >> 27;
			value *= 0x94d049bb133111ebull;
			value ^= value >> 31;

			if constexpr (sizeof(size_t) >= sizeof(ember::u64))
				return static_cast<size_t>(value);
			else
				return static_cast<size_t>(static_cast<ember::u32>(value) ^ static_cast<ember::u32>(value >> 32));
		}
	};
}
