#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/block_allocator.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory_resource>
#include <vector>

using namespace ember;
using namespace ember::jobs;

namespace
{
	struct Allocation
	{
		u8* begin	= nullptr;
		size_t size = 0;
	};
}

// Every worker bumps its own block, so two jobs never hand out the same bytes.
TEST(FrameMemoryJobs, WorkersAllocateInParallelWithoutOverlapping)
{
	BlockAllocator& frame = memory::frame_memory();
	frame.reset();

	constexpr u32 COUNT = 4096;
	std::vector<Allocation> allocations(COUNT);

	jobs::initialize({.worker_count = 4});

	auto body = [&](JobRange range)
	{
		for (u32 i = range.begin; i < range.end; ++i)
		{
			const size_t size = 16 + (i % 97) * 8;
			allocations[i]	  = {static_cast<u8*>(frame.allocate_fast(size)), size};
		}
	};

	parallel_for({.count = COUNT, .grain = 16, .name = "allocate"}, body);
	jobs::shutdown();

	std::sort(allocations.begin(), allocations.end(),
			  [](const Allocation& a, const Allocation& b) { return a.begin < b.begin; });

	for (size_t i = 0; i < allocations.size(); ++i)
	{
		ASSERT_NE(allocations[i].begin, nullptr);
		ASSERT_TRUE(frame.owns(allocations[i].begin));

		if (i > 0)
		{
			ASSERT_GE(allocations[i].begin, allocations[i - 1].begin + allocations[i - 1].size)
				<< "allocation " << i << " overlaps its neighbour";
		}
	}
}

// A container built before a wait keeps growing after it, on whichever worker the job resumed on.
TEST(FrameMemoryJobs, ContainersSurviveAFiberMigration)
{
	BlockAllocator& frame = memory::frame_memory();
	frame.reset();

	jobs::initialize({.worker_count = 4});
	std::atomic<u32> migrated = 0;

	auto body = [&](JobRange range)
	{
		std::pmr::vector<u32> values(&frame);
		const u32 before = worker_index();

		for (u32 i = 0; i < 1000; ++i)
			values.push_back(range.index);

		// A child job forces this fiber to park; it can come back on another worker.
		u32 counter = 0;
		auto child	= [&counter] { ++counter; };
		JobDef defs[]{make_job(child, "touch")};
		Batch batch(defs);
		batch.wait();

		for (u32 i = 0; i < 1000; ++i)
			values.push_back(range.index);

		ASSERT_EQ(values.size(), 2000u);
		for (u32 value : values)
			ASSERT_EQ(value, range.index);

		if (worker_index() != before)
			migrated.fetch_add(1, std::memory_order_relaxed);
	};

	parallel_for({.count = 64, .grain = 1, .name = "grow"}, body);

	EXPECT_GT(migrated.load(), 0u) << "no fiber migrated, the test proved nothing";

	jobs::shutdown();
}
