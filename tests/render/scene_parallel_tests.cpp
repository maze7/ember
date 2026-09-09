#include <ember/jobs/job_system.h>
#include <ember/render/scene.h>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <vector>

namespace
{
	using ember::f32;
	using ember::u32;
	using namespace ember::jobs;
	using namespace ember::render;

	// Distinct handles, one writer each: the contract phase 2 buys. Each job writes its own slot
	// and marks its own bit, so the only shared state is the dirty set.
	TEST(RenderSceneParallel, SetTransformRunsOnEveryWorkerAtOnce)
	{
		constexpr u32 COUNT = 1024;

		RenderScene scene;
		scene.init(COUNT);

		std::vector<RenderObjectHandle> handles(COUNT);
		for (u32 i = 0; i < COUNT; ++i)
			handles[i] = scene.create_object({.sphere = {0.0f, 0.0f, 0.0f, 1.0f}});

		scene.clear_dirty();

		JobSystem jobs({.worker_count = 4});
		struct Args
		{
			RenderScene* scene;
			std::vector<RenderObjectHandle>* handles;
		} args{&scene, &handles};

		jobs.run(
			[](void* data)
			{
				auto* args = static_cast<Args*>(data);
				auto body  = [args](JobRange range)
				{
					for (u32 i = range.begin; i < range.end; ++i)
						args->scene->set_transform(
							(*args->handles)[i],
							glm::translate(glm::mat4(1.0f), glm::vec3(f32(i), 0.0f, 0.0f)));
				};

				parallel_for({.count = COUNT, .grain = 32, .name = "set_transform"}, body);
			},
			&args);

		// Every write landed in its own slot.
		for (u32 i = 0; i < COUNT; ++i)
			ASSERT_EQ(scene.object(i).sphere.x, f32(i)) << "slot " << i;

		// And every slot is in the dirty stream exactly once, whatever order they got there in.
		std::vector<u32> slots(scene.dirty_slots().begin(), scene.dirty_slots().end());
		ASSERT_EQ(slots.size(), COUNT);

		std::sort(slots.begin(), slots.end());
		for (u32 i = 0; i < COUNT; ++i)
			ASSERT_EQ(slots[i], i);
	}
}
