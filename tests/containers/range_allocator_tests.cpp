#include <ember/containers/range_allocator.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

namespace
{
	using ember::RangeAllocator;
	using ember::u32;

	TEST(RangeAllocator, StartsEmptyAndUnusableBeforeInit)
	{
		RangeAllocator space;

		EXPECT_EQ(space.capacity(), 0u);
		EXPECT_EQ(space.allocate(1), RangeAllocator::INVALID_OFFSET);
	}

	TEST(RangeAllocator, AllocatesFirstFitFromLowOffsets)
	{
		RangeAllocator space;
		space.init(100);

		EXPECT_EQ(space.allocate(10), 0u);
		EXPECT_EQ(space.allocate(20), 10u);
		EXPECT_EQ(space.allocate(30), 30u);
		EXPECT_EQ(space.used(), 60u);
		EXPECT_EQ(space.range_count(), 1u);
	}

	TEST(RangeAllocator, FailureLeavesStateUntouched)
	{
		RangeAllocator space;
		space.init(10);

		EXPECT_EQ(space.allocate(4), 0u);
		EXPECT_EQ(space.allocate(7), RangeAllocator::INVALID_OFFSET);
		EXPECT_EQ(space.used(), 4u);
		EXPECT_EQ(space.allocate(6), 4u);
	}

	TEST(RangeAllocator, ExactFitConsumesTheFreeEntry)
	{
		RangeAllocator space;
		space.init(8);

		EXPECT_EQ(space.allocate(8), 0u);
		EXPECT_EQ(space.range_count(), 0u);
		EXPECT_EQ(space.allocate(1), RangeAllocator::INVALID_OFFSET);
	}

	TEST(RangeAllocator, FreeCoalescesWithBothNeighbours)
	{
		RangeAllocator space;
		space.init(30);

		const u32 a = space.allocate(10);
		const u32 b = space.allocate(10);
		const u32 c = space.allocate(10);

		// Freeing the middle last forces the three-way merge.
		space.free(a, 10);
		space.free(c, 10);
		EXPECT_EQ(space.range_count(), 2u);

		space.free(b, 10);
		EXPECT_EQ(space.range_count(), 1u);
		EXPECT_EQ(space.used(), 0u);
		EXPECT_EQ(space.allocate(30), 0u);
	}

	TEST(RangeAllocator, HolesAreReusedByFit)
	{
		RangeAllocator space;
		space.init(100);

		const u32 a = space.allocate(10);
		const u32 b = space.allocate(10);
		(void)b;
		const u32 c = space.allocate(10);
		(void)c;

		space.free(a, 10);

		// The freed low hole is first fit for anything that fits it.
		EXPECT_EQ(space.allocate(10), 0u);

		// Too large for any hole, so it comes from the tail.
		space.free(a, 10);
		EXPECT_EQ(space.allocate(50), 30u);
	}

	TEST(RangeAllocator, ShuffledChurnDrainsBackToOneRange)
	{
		constexpr u32 block = 16;
		constexpr u32 count = 64;

		RangeAllocator space;
		space.init(block * count);

		std::vector<u32> offsets;
		for (u32 i = 0; i < count; ++i)
			offsets.push_back(space.allocate(block));

		std::mt19937 random(0xC0FFEE);
		std::shuffle(offsets.begin(), offsets.end(), random);

		for (const u32 offset : offsets)
			space.free(offset, block);

		// Full coalescing is the invariant: any free order ends in one range.
		EXPECT_EQ(space.used(), 0u);
		EXPECT_EQ(space.range_count(), 1u);
		EXPECT_EQ(space.allocate(block * count), 0u);
	}

#ifndef NDEBUG
	TEST(RangeAllocatorDeathTest, DoubleFreeIsFatal)
	{
		EXPECT_DEATH(
			{
				RangeAllocator space;
				space.init(10);

				const u32 a = space.allocate(4);
				space.free(a, 4);
				space.free(a, 4);
			},
			"assert");
	}

	TEST(RangeAllocatorDeathTest, OverlappingFreeIsFatal)
	{
		EXPECT_DEATH(
			{
				RangeAllocator space;
				space.init(10);

				const u32 a = space.allocate(8);
				space.free(a, 4);
				space.free(a + 2, 4);
			},
			"assert");
	}
#endif
}
