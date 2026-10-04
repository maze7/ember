#pragma once

#include <ember/ecs/component.h>
#include <ember/physics/shape.h>

#include <type_traits>

/**
 * What an entity brings to collision, as three components a prefab holds: where walls stop it
 * (Collider), where it can be hit (Hurtbox) and where it hits (Hitbox). Plain data: this header
 * knows nothing of EnTT or of the space that sorts them.
 *
 * Layers are the game's own bitmask enum: what a thing is, and what it answers to.
 *
 *     enum class Layer : u32 { World = 1 << 0, Player = 1 << 1, Enemy = 1 << 2, Breakable = 1 << 3 };
 *     EMBER_ENUM_BITWISE_OPS(Layer, u32);
 */
namespace ember::physics
{
	/** A set of the game's layers. Any bitmask enum converts to it, so components name layers as the game does. */
	struct Layers
	{
		u32 bits = 0;

		constexpr Layers() noexcept = default;

		template <class E>
			requires std::is_enum_v<E>
		constexpr Layers(E layers) noexcept : bits(static_cast<u32>(layers))
		{
		}

		[[nodiscard]] constexpr bool any(Layers of) const noexcept { return (bits & of.bits) != 0; }
		[[nodiscard]] constexpr bool none() const noexcept { return bits == 0; }
		constexpr bool operator==(const Layers&) const noexcept = default;
	};

	/**
	 * The shape walls stop: an upright box, usually about the feet. `layer` is what it is to others,
	 * and `blocked_by` the layers that stop it when it moves through Space::move(): tiles and other
	 * colliders alike. One blocked by nothing passes through everything, and still stops others.
	 */
	struct Collider
	{
		Shape shape = box({16.0f, 16.0f});
		Layers layer = {};
		Layers blocked_by = {};
	};
	EMBER_COMPONENT(Collider, Sim);

	/** Where an entity can be hit: found by hitboxes that hit its layer, and by Space::hurtboxes(). */
	struct Hurtbox
	{
		Shape shape = box({16.0f, 16.0f});
		Layers layer = {};
	};
	EMBER_COMPONENT(Hurtbox, Sim);

	/**
	 * Where an entity hits: every tick its shape touches a hurtbox on one of `hits`, other than its
	 * own, the two are a Hit. Hitting nothing switches it off.
	 *
	 * `rewind` is lag compensation: how many ticks before this one its striker's screen showed the world,
	 * fractions included. Its hits are found where the hurtboxes stood then, so a blow lands on what the
	 * striker saw. Only a space that keeps a history rewinds (SpaceDef::history): a server's. At 0, the present.
	 */
	struct Hitbox
	{
		Shape shape = box({16.0f, 16.0f});
		Layers hits = {};
		f32 rewind	= 0.0f; // ticks
	};
	EMBER_COMPONENT(Hitbox, Sim);
}
