#pragma once

#include <ember/anim/components.h>
#include <ember/anim/rig.h>

namespace ember::anim
{
	/** Where a sampler finds what the ids in an entity's components name: the library, or a test's arrays. */
	class Source
	{
	public:
		/** Null while it loads, or when it failed. */
		[[nodiscard]] virtual const Sheet* sheet(u16 id) const noexcept = 0;
		[[nodiscard]] virtual const Rig* rig(u16 id) const noexcept		= 0;

		/** The sheet a rig's slot shows when the look leaves the slot alone; NO_ID before it has an id. */
		[[nodiscard]] virtual u16 default_sheet(u16 rig, Name slot) const noexcept = 0;

	protected:
		~Source() = default;
	};

	/**
	 * An entity's pose at `now`, from what its animator asks and its look holds, their ids resolved.
	 * Nothing but the time moves it, so a frame drawn twice or a rollback draws the same; events are
	 * the ones its clips passed since `previous`, the frame before. Everything is worked out facing
	 * east and mirrored as it is written.
	 */
	void sample(const Source& source, const Animator& animator, const Look& look, f64 now, f64 previous,
				Pose& pose) noexcept;
}
