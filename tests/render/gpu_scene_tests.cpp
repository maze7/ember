#include <ember/render/gpu_scene.h>

#include <gtest/gtest.h>

#include <vector>

namespace
{
	using ember::u32;
	using ember::render::for_each_slot_run;

	struct SlotRun
	{
		u32 first = 0;
		u32 count = 0;

		bool operator==(const SlotRun&) const = default;
	};

	[[nodiscard]] std::vector<SlotRun> collect(std::initializer_list<u32> slots)
	{
		const std::vector<u32> sorted(slots);
		std::vector<SlotRun> runs;

		for_each_slot_run(
			{sorted.data(), sorted.size()}, [&](u32 first, u32 count) { runs.push_back({first, count}); });

		return runs;
	}

	TEST(GpuScene, EmptySlotListProducesNoRuns) { EXPECT_TRUE(collect({}).empty()); }

	TEST(GpuScene, SingleSlotIsOneRun) { EXPECT_EQ(collect({5}), (std::vector<SlotRun>{{5, 1}})); }

	TEST(GpuScene, ConsecutiveSlotsMergeIntoOneRun)
	{
		EXPECT_EQ(collect({3, 4, 5, 6}), (std::vector<SlotRun>{{3, 4}}));
	}

	TEST(GpuScene, GapsSplitRuns)
	{
		EXPECT_EQ(collect({0, 1, 2, 5, 7, 8}), (std::vector<SlotRun>{{0, 3}, {5, 1}, {7, 2}}));
	}

	TEST(GpuScene, AlternatingSlotsDegradeToSingleRuns)
	{
		EXPECT_EQ(collect({0, 2, 4, 6}), (std::vector<SlotRun>{{0, 1}, {2, 1}, {4, 1}, {6, 1}}));
	}

	TEST(GpuScene, RunAtTheEndTerminatesCleanly) { EXPECT_EQ(collect({9, 10}), (std::vector<SlotRun>{{9, 2}})); }
}
