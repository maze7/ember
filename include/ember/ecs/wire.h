#pragma once

#include <ember/ecs/component.h>
#include <ember/net/serialize.h>

#include <boost/pfr/core.hpp>
#include <glm/gtc/quaternion.hpp>

#include <concepts>
#include <cstring>
#include <utility>

/**
 * How a component crosses the wire, and how it is drawn between two updates.
 *
 * A Replicated component crosses with its own serialize(), as commands and messages do: the
 * compressed floats, ranges and bit widths are the game's to choose, and registering the component
 * without one does not compile. A tag needs none: having it is all it says.
 *
 * An Interpolated component is drawn field by field (Boost.PFR reads the fields): floats, float
 * vectors and nested plain structs move, a quaternion turns the short way, and every other field
 * holds the earlier sample's value until the later one's tick. One with its own static
 * interpolate(from, to, t) is drawn with that instead, an angle that wraps say.
 */
namespace ember::ecs
{
	/** The largest Replicated component, in memory and on the wire. */
	inline constexpr u32 MAX_REPLICATED_BYTES = 64;
	inline constexpr u32 MAX_REPLICATED_BITS  = 512;

	/** A type with serialize(): what a Replicated component must be. */
	template <class T>
	concept Serializable = requires(T& value, serialize::WriteStream& writer, serialize::ReadStream& reader) {
		{ value.serialize(writer) } -> std::same_as<bool>;
		{ value.serialize(reader) } -> std::same_as<bool>;
	};

	/** A type with its own static interpolate(from, to, t). */
	template <class T>
	concept OwnInterpolate = requires(const T& from, const T& to, f32 t) {
		{ T::interpolate(from, to, t) } -> std::same_as<T>;
	};

	namespace detail
	{
		template <class> inline constexpr bool is_float_vec = false;
		template <glm::length_t L, class F, glm::qualifier Q>
		inline constexpr bool is_float_vec<glm::vec<L, F, Q>> = std::is_floating_point_v<F>;

		template <class> inline constexpr bool is_quat									   = false;
		template <class F, glm::qualifier Q> inline constexpr bool is_quat<glm::qua<F, Q>> = true;

		/** One value between two others: what moves moves, and the rest holds until t reaches 1. */
		template <class F> [[nodiscard]] F interpolate_value(const F& from, const F& to, f32 t) noexcept
		{
			if constexpr (OwnInterpolate<F>)
				return F::interpolate(from, to, t);
			else if constexpr (std::is_floating_point_v<F>)
				return from + (to - from) * static_cast<F>(t);
			else if constexpr (is_float_vec<F>)
				return from + (to - from) * static_cast<typename F::value_type>(t);
			else if constexpr (is_quat<F>)
				return glm::slerp(from, to, static_cast<typename F::value_type>(t));
			else if constexpr (std::is_aggregate_v<F> && !std::is_array_v<F> && !std::is_empty_v<F>)
			{
				F value = from;
				[&]<size_t... Is>(std::index_sequence<Is...>)
				{
					((boost::pfr::get<Is>(value) =
						  interpolate_value(boost::pfr::get<Is>(from), boost::pfr::get<Is>(to), t)),
					 ...);
				}(std::make_index_sequence<boost::pfr::tuple_size_v<F>>{});
				return value;
			}
			else
				return t < 1.0f ? from : to;
		}

		/** A component's bytes into a value of its own type. A tag has no bytes to copy. */
		template <class T> [[nodiscard]] T load(const void* bytes) noexcept
		{
			T value{};
			if constexpr (!std::is_empty_v<T>)
				std::memcpy(&value, bytes, sizeof(T));
			return value;
		}

		template <class T> bool write_component(serialize::WriteStream& stream, const void* bytes) noexcept
		{
			if constexpr (Serializable<T>)
			{
				T copy = load<T>(bytes);
				return copy.serialize(stream);
			}
			else
			{
				(void)stream; // a tag: being there is all it says
				(void)bytes;
				return true;
			}
		}

		template <class T> bool read_component(serialize::ReadStream& stream, void* bytes) noexcept
		{
			if constexpr (Serializable<T>)
			{
				T copy{};
				if (!copy.serialize(stream))
					return false;

				std::memcpy(bytes, &copy, sizeof(T));
			}
			else
			{
				(void)stream;
				(void)bytes;
			}
			return true;
		}

		template <class T> void interpolate_component(const void* from, const void* to, f32 t, void* out) noexcept
		{
			const T value = interpolate_value(load<T>(from), load<T>(to), t);
			std::memcpy(out, &value, sizeof(T));
		}
	}
}
