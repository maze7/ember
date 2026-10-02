#pragma once

#include <ember/core/bitmask.h>
#include <ember/core/common.h>

#include <string_view>
#include <type_traits>

/**
 * Ember::Ecs is the engine's entity component system, built on top of EnTT. A game describes its
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

	/** Everything EMBER_COMPONENT says about a type: its name and its kind. */
	template <class T> struct ComponentDescription
	{
		std::string_view name;
		Kind kind = Kind::None;
	};

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
}

/**
 * Says what a component is, once: where it lives (Sim, Server, Client) and how it crosses the wire
 * (Replicated, Interpolated, Predicted, OwnerOnly). Worlds, prefabs, the net layer and the compile
 * time checks all read this. Put it after the struct, in the struct's namespace.
 *
 *     struct HealthComponent
 *     {
 *         u16 current = 50;
 *         u16 max     = 50;
 *
 *         template <class Stream> bool serialize(Stream& stream)
 *         {
 *             serialize_bits(stream, current, 16);
 *             serialize_bits(stream, max, 16);
 *             return true;
 *         }
 *     };
 *     EMBER_COMPONENT(HealthComponent, Replicated);
 *
 * A Replicated component says how it crosses the wire with serialize() (ember/net/serialize.h). An
 * Interpolated one is drawn between two updates field by field, floats and float vectors moving and
 * everything else held, unless it has its own static T interpolate(const T& from, const T& to, f32 t).
 */
#define EMBER_COMPONENT(Type, kinds)                                                                                   \
	[[nodiscard]] consteval auto ember_component(const Type*) noexcept                                                 \
	{                                                                                                                  \
		using enum ::ember::ecs::Kind;                                                                                 \
		static_assert(std::is_trivially_copyable_v<Type>, #Type ": components are plain data");                        \
		static_assert(::ember::ecs::one_home(::ember::ecs::normalize(kinds)),                                          \
					  #Type ": exactly one of Sim, Server or Client, and Replicated or a net flag means Sim");         \
		return ::ember::ecs::ComponentDescription<Type>{#Type, ::ember::ecs::normalize(kinds)};                        \
	}
