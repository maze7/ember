#include <ember/core/profile.h>
#include <ember/physics/systems.h>

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
			// However many it touches: a blast over a crowd hits all of it.
			hits.m_touched.clear();
			space.hurtboxes(hitbox.shape, hitbox.layers, hits.m_touched);
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
			hit.began = before == hits.m_before.end() || key(*before) != key(hit);
		}
	}

	void add_resources(ecs::World& world, const SpaceDef& def) noexcept
	{
		world.add_resource<Space>(def);
		world.add_resource<Hits>();
	}
}
