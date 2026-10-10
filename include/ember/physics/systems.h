#pragma once

#include <ember/ecs/system.h>
#include <ember/physics/space.h>

namespace ember::physics
{
	/** A hitbox and a hurtbox that touch this tick. */
	struct Hit
	{
		ecs::Entity hitbox	= ecs::NO_ENTITY; // the entity whose hitbox it is
		ecs::Entity hurtbox = ecs::NO_ENTITY; // the entity it hit
		u32 ticks			= 0;			  // ticks the two had touched before this one, 0 the tick they met
		bool began			= false;		  // the Hit began this tick
	};

	/**
	 * This tick's hits, in entity order: a world resource the systems that react take const. A hit is
	 * there every tick the two touch; `began` marks the first.
	 */
	class Hits final
	{
	public:
		[[nodiscard]] const Hit* begin() const noexcept { return m_hits.data(); }
		[[nodiscard]] const Hit* end() const noexcept { return m_hits.data() + m_hits.size(); }
		[[nodiscard]] u32 count() const noexcept { return static_cast<u32>(m_hits.size()); }

	private:
		friend void find_hits(const Space& space, Hits& hits);

		Vector<Hit> m_hits{&memory::heap(MemoryTag::Physics)};
		Vector<Hit> m_before{&memory::heap(MemoryTag::Physics)};
		Vector<Touch> m_touched{&memory::heap(MemoryTag::Physics)}; // one hitbox's, reused
	};

	namespace detail
	{
		template <class Member> struct OwnerOf;
		template <class C> struct OwnerOf<glm::vec2 C::*>
		{
			using type = C;
		};
	}

	/** The component a game keeps positions in, from a pointer to its vec2: &Position::value. */
	template <auto Position> using Positioned = typename detail::OwnerOf<decltype(Position)>::type;

	/**
	 * Simulate: every collider, hurtbox and hitbox into the space, where its entity stands now. What
	 * moves after it is found where it stood until the next tick's sync, so run it after the systems
	 * that move things.
	 */
	template <auto Position>
	void sync(ecs::View<const Positioned<Position>, const Collider> colliders,
			  ecs::View<const Positioned<Position>, const Hurtbox> hurtboxes,
			  ecs::View<const Positioned<Position>, const Hitbox> hitboxes, Space& space)
	{
		space.clear();
		for (auto [entity, position, collider] : colliders.each())
			space.add(entity, position.*Position, collider);
		for (auto [entity, position, hurtbox] : hurtboxes.each())
			space.add(entity, position.*Position, hurtbox);
		for (auto [entity, position, hitbox] : hitboxes.each())
			space.add(entity, position.*Position, hitbox);
		space.build();
	}

	/**
	 * Simulate: the hitboxes and the hurtboxes that touch, into Hits. A hitbox that rewinds finds the hurtboxes
	 * where they stood that many ticks ago.
	 */
	void find_hits(const Space& space, Hits& hits);

	/**
	 * One tick of a body: still while frozen, else moved by velocity + push, stopped and slid by what blocks its
	 * collider (none: it passes through everything), the blocked axis of both zeroed, then the push lerped toward
	 * rest and snapped to it under REST_SPEED. THe same answer from the same inputs on every machine, so a replay
	 * repeats it. A game's integration system calls it for each body it simulates.
	 */
	Moved step(const Space& space, ecs::Entity self, Body& body, glm::vec2& position, const Collider* collider,
			   u32 tick, f32 dt) noexcept;

	/**
	 * The components and the two systems, in a stage of the game's after the one that moves things.
	 * Position says where the game keeps an entity's position:
	 *
	 *     physics::register_systems<&Position::value>(registry, Stage::Collide);
	 */
	template <auto Position, class Stage> void register_systems(ecs::Registry& registry, Stage stage) noexcept
	{
		registry.components<Collider, Hurtbox, Hitbox, Body>();
		registry.template simulate<&sync<Position>>(stage);
		registry.template simulate<&find_hits>(stage);
	}

	/** A world's space and its hits, which the systems take. */
	void add_resources(ecs::World& world, const SpaceDef& def = {}) noexcept;
}
