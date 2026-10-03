#pragma once

#include <ember/anim/components.h>
#include <ember/anim/library.h>
#include <ember/ecs/system.h>

namespace ember::anim
{
	/** The time animation runs on, in seconds: this frame's and the one before, so events fire once. */
	struct Clock
	{
		f64 now		 = 0.0;
		f64 previous = 0.0;

		void advance(f64 dt) noexcept { set(now + dt); }

		void set(f64 time) noexcept
		{
			previous = now;
			now		 = time;
		}
	};

	/** Present: the library's ids for the paths an animator or look names, once each. One thread: the library grows. */
	void resolve(ecs::View<Animator, Look> animated, Library& library);

	/** Present: every animated entity's pose, from its animator and look, on every core. */
	void animate(ecs::View<const Animator, const Look, Pose> animated, const Library& library, const Clock& clock);

	/**
	 * The components and the two systems. Register it after the Present systems that ask for clips
	 * and before those that read poses, which is the order they run in.
	 */
	void register_systems(ecs::Registry& registry) noexcept;

	/** A world's library and clock, which the systems take. Advance the clock before Present runs. */
	void add_resources(ecs::World& world, AssetManager& assets) noexcept;
}
