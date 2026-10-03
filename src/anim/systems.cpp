#include <ember/anim/systems.h>

namespace ember::anim
{
	void resolve(ecs::View<Animator, Look> animated, Library& library)
	{
		for (auto [entity, animator, look] : animated.each())
		{
			if (animator.rig_id == NO_ID)
				animator.rig_id = library.rig_id(animator.rig);

			for (u32 i = 0; i < look.count; ++i)
			{
				Look::Slot& slot = look.slots[i];
				if (slot.id == NO_ID)
					slot.id = slot.rig ? library.rig_id(slot.asset) : library.sheet_id(slot.asset);
			}
		}

		library.refresh();
	}

	void animate(ecs::View<const Animator, const Look, Pose> animated, const Library& library, const Clock& clock)
	{
		animated.parallel_each([&](ecs::Entity, const Animator& animator, const Look& look, Pose& pose)
							   { sample(library, animator, look, clock.now, clock.previous, pose); }, 64);
	}

	void register_systems(ecs::Registry& registry) noexcept
	{
		registry.components<Look, Animator, Pose>();
		registry.present<resolve>();
		registry.present<animate>();
	}

	void add_resources(ecs::World& world, AssetManager& assets) noexcept
	{
		world.add_resource<Library>(assets);
		world.add_resource<Clock>();
	}
}
