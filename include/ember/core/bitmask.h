#pragma once

#include <cstdint>

#define EMBER_ENUM_BITWISE_OPS(EnumType, UnderlyingType)                                                               \
	[[nodiscard]] constexpr EnumType operator|(EnumType lhs, EnumType rhs) noexcept                                    \
	{                                                                                                                  \
		return static_cast<EnumType>(static_cast<UnderlyingType>(lhs) | static_cast<UnderlyingType>(rhs));             \
	}                                                                                                                  \
                                                                                                                       \
	[[nodiscard]] constexpr EnumType operator&(EnumType lhs, EnumType rhs) noexcept                                    \
	{                                                                                                                  \
		return static_cast<EnumType>(static_cast<UnderlyingType>(lhs) & static_cast<UnderlyingType>(rhs));             \
	}                                                                                                                  \
                                                                                                                       \
	[[nodiscard]] constexpr EnumType operator^(EnumType lhs, EnumType rhs) noexcept                                    \
	{                                                                                                                  \
		return static_cast<EnumType>(static_cast<UnderlyingType>(lhs) ^ static_cast<UnderlyingType>(rhs));             \
	}                                                                                                                  \
                                                                                                                       \
	constexpr EnumType& operator|=(EnumType& lhs, EnumType rhs) noexcept                                               \
	{                                                                                                                  \
		lhs = lhs | rhs;                                                                                               \
		return lhs;                                                                                                    \
	}                                                                                                                  \
                                                                                                                       \
	constexpr EnumType& operator&=(EnumType& lhs, EnumType rhs) noexcept                                               \
	{                                                                                                                  \
		lhs = lhs & rhs;                                                                                               \
		return lhs;                                                                                                    \
	}                                                                                                                  \
                                                                                                                       \
	constexpr EnumType& operator^=(EnumType& lhs, EnumType rhs) noexcept                                               \
	{                                                                                                                  \
		lhs = lhs ^ rhs;                                                                                               \
		return lhs;                                                                                                    \
	}                                                                                                                  \
                                                                                                                       \
	[[nodiscard]] constexpr EnumType operator~(EnumType value) noexcept                                                \
	{                                                                                                                  \
		return static_cast<EnumType>(~static_cast<UnderlyingType>(value));                                             \
	}                                                                                                                  \
                                                                                                                       \
	[[nodiscard]] constexpr bool any(EnumType value) noexcept { return static_cast<UnderlyingType>(value) != 0; }      \
                                                                                                                       \
	[[nodiscard]] constexpr bool none(EnumType value) noexcept { return static_cast<UnderlyingType>(value) == 0; }     \
                                                                                                                       \
	[[nodiscard]] constexpr bool has_any(EnumType value, EnumType flags) noexcept { return any(value & flags); }       \
                                                                                                                       \
	[[nodiscard]] constexpr bool has_all(EnumType value, EnumType flags) noexcept { return (value & flags) == flags; }
