#include <ember/core/profile.h>
#include <ember/physics/systems.h>

#include <glm/geometric.hpp>

#include <algorithm>

namespace ember::physics
{
	namespace
	{
		[[nodiscard]] u64 key(const Hit& hit) noexcept
		{
			return (static_cast<u64>(entt::to_integral(hit.hitbox)) << 32) | entt::to_integral(hit.hurtbox);
		}
	}

	void find_hits(const Space& space, Hits& hits)
	{
		EMBER_PROFILE_SCOPE_C("physics::find_hits", PROFILE_COLOR_PHYSICS);
		std::swap(hits.m_hits, hits.m_before);
		hits.m_hits.clear();

		for (const Proxy& hitbox : space.hitboxes())
		{
			// However many it touches: a blast over a crowd hits all of it. One that rewinds finds them where its
			// striker saw them.
			hits.m_touched.clear();
			space.hurtboxes(hitbox.shape, hitbox.layers, hitbox.rewind, hits.m_touched);
			for (const Touch& touch : hits.m_touched)
				if (touch.entity != hitbox.entity)
					hits.m_hits.push_back({.hitbox = hitbox.entity, .hurtbox = touch.entity});
		}

		// In entity order, so the tick before's can be walked alongside: a hit that was not there began.
		std::sort(hits.m_hits.begin(), hits.m_hits.end(), [](const Hit& a, const Hit& b) { return key(a) < key(b); });

		auto before = hits.m_before.begin();
		for (Hit& hit : hits.m_hits)
		{
			while (before != hits.m_before.end() && key(*before) < key(hit))
				++before;

			const bool touched = before != hits.m_before.end() && key(*before) == key(hit);
			hit.ticks		   = touched ? before->ticks + 1 : 0;
			hit.began		   = !touched;
		}
	}

	Moved step(const Space& space, ecs::Entity self, Body& body, glm::vec2& position, const Collider* collider,
			   u32 tick, f32 dt) noexcept
	{
		if (tick < body.frozen_until)
			return {.position = position};

		const glm::vec2 delta = (body.velocity + body.push) * dt;
		const Moved moved =
			collider != nullptr ? space.move(self, *collider, position, delta) : Moved{.position = position + delta};

		position = moved.position;

		// What a wall stops is gone: a dash into one ends there, and a shove along one slides.
		if (moved.normal.x != 0.0f)
			body.velocity.x = body.push.x = 0.0f;
		if (moved.normal.y != 0.0f)
			body.velocity.y = body.push.y = 0.0f;

		// After the move, so a kick carries its whole first tick; to rest exactly, so a body at rest stops changing.
		body.push *= 1.0f - body.push_drag;
		if (glm::dot(body.push, body.push) < REST_SPEED * REST_SPEED)
			body.push = {};
		return moved;
	}

	void add_resources(ecs::World& world, const SpaceDef& def) noexcept
	{
		world.add_resource<Space>(def);
		world.add_resource<Hits>();
	}
}
