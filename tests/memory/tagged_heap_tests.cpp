#include <ember/memory/tagged_heap.h>

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

using namespace ember;

namespace
{
	constexpr size_t BLOCK	  = 64_kb;
	constexpr size_t CAPACITY = 16 * BLOCK;

	constexpr HeapTag TAG_A = heap_tag(1);
	constexpr HeapTag TAG_B = heap_tag(2);

	class BlockHeap : public ::testing::Test
	{
	protected:
		void SetUp() override { ASSERT_TRUE(heap.init(CAPACITY, BLOCK, MemoryTag::Engine)); }
		void TearDown() override { heap.shutdown(); }

		TaggedHeap heap;
	};
}

TEST_F(BlockHeap, CutsTheReservationIntoBlocks)
{
	EXPECT_EQ(heap.block_count(), 16u);
	EXPECT_EQ(heap.block_size(), BLOCK);
	EXPECT_EQ(heap.blocks_in_use(), 0u);
}

TEST_F(BlockHeap, HandsOutBlocksUnderATag)
{
	void* block = heap.allocate_blocks(1, TAG_A);

	ASSERT_NE(block, nullptr);
	EXPECT_TRUE(heap.owns(block));
	EXPECT_EQ(heap.blocks_in_use(), 1u);
}

TEST_F(BlockHeap, ARunIsConsecutive)
{
	auto* run = static_cast<u8*>(heap.allocate_blocks(3, TAG_A));

	ASSERT_NE(run, nullptr);
	EXPECT_EQ(heap.blocks_in_use(), 3u);
	EXPECT_TRUE(heap.owns(run + 3 * BLOCK - 1));
}

TEST_F(BlockHeap, FreeingOneTagLeavesTheOtherAlone)
{
	void* a = heap.allocate_blocks(4, TAG_A);
	void* b = heap.allocate_blocks(4, TAG_B);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);

	heap.free_blocks(TAG_A);

	EXPECT_EQ(heap.blocks_in_use(), 4u);
	EXPECT_EQ(heap.allocate_blocks(4, TAG_A), a); // the hint rewinds, so the same blocks come back
}

// The reason the heap scans a tag table instead of bumping a cursor: freed blocks have to be
// available for runs again, wherever they sit in the pool.
TEST_F(BlockHeap, ReusesFreedBlocksForLaterRuns)
{
	ASSERT_NE(heap.allocate_blocks(8, TAG_A), nullptr);
	ASSERT_NE(heap.allocate_blocks(8, TAG_B), nullptr);
	EXPECT_EQ(heap.blocks_in_use(), 16u);

	heap.free_blocks(TAG_A);

	// Nothing was ever appended to a free list, and no cursor rewound, yet a full run fits.
	EXPECT_NE(heap.allocate_blocks(8, TAG_A), nullptr);
	EXPECT_EQ(heap.blocks_in_use(), 16u);
}

TEST_F(BlockHeap, ReportsFullInsteadOfOverAllocating)
{
	ASSERT_NE(heap.allocate_blocks(16, TAG_A), nullptr);

	EXPECT_EQ(heap.allocate_blocks(1, TAG_B), nullptr);
	EXPECT_EQ(heap.allocate_blocks(17, TAG_B), nullptr); // longer than the heap
}

TEST_F(BlockHeap, FragmentationOnlyBlocksRunsThatCannotFit)
{
	// Take every block, alternating tags, then give every other one back.
	for (u32 i = 0; i < 16; ++i)
		ASSERT_NE(heap.allocate_blocks(1, (i % 2 == 0) ? TAG_A : TAG_B), nullptr);

	heap.free_blocks(TAG_B);

	EXPECT_EQ(heap.allocate_blocks(2, TAG_B), nullptr); // no two free blocks are adjacent
	EXPECT_NE(heap.allocate_blocks(1, TAG_B), nullptr);
}

TEST_F(BlockHeap, ManyThreadsClaimDistinctBlocks)
{
	constexpr u32 THREADS = 8;

	std::atomic<u32> ready = 0;
	std::vector<std::thread> pool;
	std::vector<std::vector<void*>> claimed(THREADS);

	for (u32 t = 0; t < THREADS; ++t)
		pool.emplace_back(
			[&, t]
			{
				ready.fetch_add(1);
				while (ready.load() < THREADS)
					;

				for (u32 i = 0; i < 2; ++i)
					if (void* block = heap.allocate_blocks(1, heap_tag(t + 1)))
						claimed[t].push_back(block);
			});

	for (std::thread& thread : pool)
		thread.join();

	std::set<void*> unique;
	size_t total = 0;

	for (const std::vector<void*>& blocks : claimed)
		for (void* block : blocks)
		{
			++total;
			EXPECT_TRUE(unique.insert(block).second) << "two threads were handed the same block";
		}

	EXPECT_EQ(total, 16u); // every block went out exactly once
	EXPECT_EQ(heap.blocks_in_use(), 16u);
}
