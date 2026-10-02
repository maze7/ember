#pragma once

#include <ember/core/bitmask.h>
#include <ember/core/common.h>
#include <ember/core/json.h>

#include <glm/fwd.hpp>

#include <limits>
#include <string_view>
#include <tuple>
#include <type_traits>

/**
 * Ember::Ecs is the engine's entity component system, build on top of EnTT. A game describes its
 * components with EMBER_COMPONENT and writes its systems as free functions; the engine keeps the worlds,
 * makes entities from prefabs and runs the systems.
 *
 * This header is what a component's own header includes. It knows nothing of EnTT.
 */
namespace ember::ecs
{
	/**
	 * Where a component lives and how it crosses the wire. Exactly one of Sim, Server or Client;
	 * the net flags apply to Sim components, and each implies Replicated, which implies Sim.
	 */
	enum class Kind : u16
	{
		None   = 0,
		Sim	   = 1 << 0, // game state: the server's truth, and what a client needs of it
		Server = 1 << 1, // only on the server: AI, spawners, loot tables
		Client = 1 << 2, // only on a client: sprites, sounds, effects, interface

		Replicated	 = 1 << 3, // the server's value reaches every client that can see the entity
		Interpolated = 1 << 4, // clients draw it between updates
		Predicted	 = 1 << 5, // the owner's client simulates it ahead, and is corrected
		OwnerOnly	 = 1 << 6, // only the owner's client receives it
	};

	EMBER_ENUM_BITWISE_OPS(Kind, u16);

	/** A kind with what its flags imply: a net flag makes it Replicated, and Replicated makes Sim. */
	constexpr Kind normalize(Kind kind) noexcept
	{
		if (has_any(kind, Kind::Interpolated | Kind::Predicted | Kind::OwnerOnly))
			kind |= Kind::Replicated;
		if (has_any(kind, Kind::Replicated))
			kind |= Kind::Sim;

		return kind;
	}

	/** Whether a kind says exactly one of Sim, Server and Client. */
	constexpr bool one_home(Kind kind) noexcept
	{
		const u16 homes = static_cast<u16>(kind & (Kind::Sim | Kind::Server | Kind::Client));
		return homes != 0 && (homes & (homes - 1)) == 0;
	}

	/** One field of a component, as EMBER_COMPONENT lists it: for prefabs, the editor and the wire. */
	template <class C, class F> struct Field
	{
		std::string_view name;
		F C::* member = nullptr;
	};

	/** Everything EMBER_COMPONENT says about a type: its name, its kind, and its fields. */
	template <class T, class Fields> struct ComponentDescription
	{
		std::string_view name;
		Kind kind = Kind::None;
		Fields fields;
	};

	namespace detail
	{
		template <class T, class Fields>
		consteval ComponentDescription<T, Fields> describe(std::string_view name, Kind kind, Fields fields) noexcept
		{
			return {name, kind, fields};
		}
	}

	/** A type EMBER_COMPONENT describes. */
	template <class T>
	concept Component = requires { ember_component(static_cast<const T*>(nullptr)); };

	template <Component T> inline constexpr auto description_of = ember_component(static_cast<const T*>(nullptr));
	template <Component T> inline constexpr Kind kind_of		= description_of<T>.kind;

	template <class T>
	concept SimComponent = Component<T> && has_any(kind_of<T>, Kind::Sim);

	template <class T>
	concept ServerComponent = Component<T> && has_any(kind_of<T>, Kind::Server);

	template <class T>
	concept ClientComponent = Component<T> && has_any(kind_of<T>, Kind::Client);

	template <class T>
	concept ReplicatedComponent = Component<T> && has_any(kind_of<T>, Kind::Replicated);

	namespace detail
	{
		template <class> struct is_glm_vec : std::false_type
		{
		};

		template <glm::length_t L, class T, glm::qualifier Q> struct is_glm_vec<glm::vec<L, T, Q>> : std::true_type
		{
		};

		/**
		 * A field from a prefab file: numbers, bools, enums by name and vectors as arrays. False leaves
		 * the value alone: a missing value, a wrong type, a number out of the field's range, or a field
		 * type prefabs cannot set.
		 */
		template <class F> [[nodiscard]] bool read_value(F& value, JsonValue json) noexcept
		{
			if constexpr (std::is_same_v<F, bool> || std::is_same_v<F, f32> || std::is_same_v<F, f64>)
			{
				return json.read(value);
			}
			else if constexpr (std::is_enum_v<F>)
			{
				return json.read_enum(value);
			}
			else if constexpr (std::is_integral_v<F>)
			{
				i64 wide = 0;
				if (!json.read(wide))
					return false;

				if constexpr (std::is_signed_v<F>)
				{
					if (wide < static_cast<i64>(std::numeric_limits<F>::min()) ||
						wide > static_cast<i64>(std::numeric_limits<F>::max()))
						return false;
				}
				else if (wide < 0 || static_cast<u64>(wide) > static_cast<u64>(std::numeric_limits<F>::max()))
				{
					return false;
				}

				value = static_cast<F>(wide);
				return true;
			}
			else if constexpr (is_glm_vec<F>::value)
			{
				f32 numbers[4] = {};
				if (json.numbers(Span<f32>(numbers, F::length())) != static_cast<u32>(F::length()))
					return false;

				for (glm::length_t i = 0; i < F::length(); ++i)
					value[i] = static_cast<typename F::value_type>(numbers[i]);
				return true;
			}
			else
			{
				return false;
			}
		}
	}
}

#define EMBER_ECS_PARENS ()
#define EMBER_ECS_EXPAND(...) EMBER_ECS_EXPAND3(EMBER_ECS_EXPAND3(EMBER_ECS_EXPAND3(EMBER_ECS_EXPAND3(__VA_ARGS__))))
#define EMBER_ECS_EXPAND3(...) EMBER_ECS_EXPAND2(EMBER_ECS_EXPAND2(EMBER_ECS_EXPAND2(EMBER_ECS_EXPAND2(__VA_ARGS__))))
#define EMBER_ECS_EXPAND2(...) EMBER_ECS_EXPAND1(EMBER_ECS_EXPAND1(EMBER_ECS_EXPAND1(EMBER_ECS_EXPAND1(__VA_ARGS__))))
#define EMBER_ECS_EXPAND1(...) __VA_ARGS__
#define EMBER_ECS_FOR_EACH(macro, type, ...)                                                                           \
	__VA_OPT__(EMBER_ECS_EXPAND(EMBER_ECS_FOR_EACH_HELPER(macro, type, __VA_ARGS__)))
#define EMBER_ECS_FOR_EACH_HELPER(macro, type, first, ...)                                                             \
	macro(type, first) __VA_OPT__(, EMBER_ECS_FOR_EACH_AGAIN EMBER_ECS_PARENS(macro, type, __VA_ARGS__))
#define EMBER_ECS_FOR_EACH_AGAIN() EMBER_ECS_FOR_EACH_HELPER
#define EMBER_ECS_FIELD(type, name)                                                                                    \
	::ember::ecs::Field<type, decltype(type::name)> { #name, &type::name }

/**
 * Says what a component is, once: where it lives (Sim, Server, Client), how it crosses the wire
 * (Replicated, Interpolated, Predicted, OwnerOnly) and its fields. Prefabs, the editor, the wire
 * and the compile time checks all read this. Put it after the struct, in the struct's namespace;
 * MSVC needs /Zc:preprocessor for the field list.
 *
 *     struct HealthComponent
 *     {
 *         u16 current = 50;
 *         u16 max     = 50;
 *     };
 *     EMBER_COMPONENT(HealthComponent, Replicated, current, max);
 */
#define EMBER_COMPONENT(Type, kinds, ...)                                                                              \
	[[nodiscard]] consteval auto ember_component(const Type*) noexcept                                                 \
	{                                                                                                                  \
		using enum ::ember::ecs::Kind;                                                                                 \
		static_assert(std::is_trivially_copyable_v<Type>, #Type ": components are plain data");                        \
		static_assert(::ember::ecs::one_home(::ember::ecs::normalize(kinds)),                                          \
					  #Type ": exactly one of Sim, Server or Client, and Replicated or a net flag means Sim");         \
		return ::ember::ecs::detail::describe<Type>(                                                                   \
			#Type, ::ember::ecs::normalize(kinds),                                                                     \
			std::make_tuple(EMBER_ECS_FOR_EACH(EMBER_ECS_FIELD, Type, __VA_ARGS__)));                                  \
	}
