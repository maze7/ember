#pragma once

#include <ember/physics/systems.h>
#include <ember/script/components.h>
#include <ember/script/host.h>

namespace ember::script
{
	/**
	 * Simulate, the server's: every Trigger asks the space for the hurtboxes its shape touches about the entity,
	 * and the host raises "entered" and "left" on it, for its stategraph and its stories. After the collide stage
	 * that builds the space. Position says where the game keeps an entity's position:
	 *
	 *     registry.simulate<&script::sense_triggers<&Position::value>>(Stage::React, ecs::Where::Server);
	 */
	template <auto Position>
	void sense_triggers(ecs::View<const physics::Positioned<Position>, const Trigger> triggers,
						const physics::Space& space, Host& host)
	{
		Vector<physics::Touch> inside(&memory::heap(MemoryTag::Scripting));
		for (auto [entity, position, trigger] : triggers.each())
		{
			inside.clear();
			space.hurtboxes(trigger.shape.at(position.*Position), trigger.layers, inside);
			host.sense(entity, Span<const physics::Touch>(inside.data(), inside.size()));
		}
		host.sense_done();
	}
}
