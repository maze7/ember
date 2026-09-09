#include <ember/memory/pmr/block_allocator.h>
#include <ember/memory/tagged_heap.h>

#include <gtest/gtest.h>

#include <memory_resource>
#include <vector>

using namespace ember;

namespace
{
	// Small enough that a handful of allocations cross blocks, big enough to stay realistic.
	constexpr size_t BLOCK	  = 64_kb;
	constexpr size_t CAPACITY = 16 * BLOCK;

	constexpr HeapTag TEST_TAG = heap_tag(1);

	class Blocks : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			ASSERT_TRUE(heap.init(CAPACITY, BLOCK, MemoryTag::Engine));
			frame.init(heap, TEST_TAG);
		}

		void TearDown() override
		{
			frame.shutdown();
			heap.shutdown();
		}

		TaggedHeap heap;
		BlockAllocator frame;
	};
}

TEST_F(Blocks, TakesOneBlockAtATime)
{
	EXPECT_EQ(heap.blocks_in_use(), 0u);

	void* first = frame.allocate_fast(16);
	EXPECT_TRUE(frame.owns(first));
	EXPECT_EQ(heap.blocks_in_use(), 1u);

	// The rest of the block is bumped without going back to the heap.
	for (int i = 0; i < 64; ++i)
		EXPECT_TRUE(frame.owns(frame.allocate_fast(256)));

	EXPECT_EQ(heap.blocks_in_use(), 1u);
}

TEST_F(Blocks, AllocationsDoNotOverlap)
{
	auto* a = static_cast<u8*>(frame.allocate_fast(100));
	auto* b = static_cast<u8*>(frame.allocate_fast(100));

	EXPECT_GE(b, a + 100);
}

TEST_F(Blocks, HonoursAlignment)
{
	(void)frame.allocate_fast(1); // leave the cursor odd
	for (size_t alignment : {size_t{16}, size_t{64}, size_t{256}, size_t{4096}})
	{
		void* ptr = frame.allocate_fast(32, alignment);
		EXPECT_TRUE(is_aligned(ptr, alignment)) << "alignment " << alignment;
	}
}

TEST_F(Blocks, TakesANewBlockWhenTheCurrentOneIsFull)
{
	void* first = frame.allocate_fast(BLOCK / 2);
	EXPECT_EQ(heap.blocks_in_use(), 1u);

	void* second = frame.allocate_fast(BLOCK / 2 + 64); // no longer fits
	EXPECT_EQ(heap.blocks_in_use(), 2u);
	EXPECT_TRUE(frame.owns(second));
	EXPECT_NE(first, second);
}

TEST_F(Blocks, AnOversizeAllocationSpansNeighbouringBlocks)
{
	auto* ptr = static_cast<u8*>(frame.allocate_fast(BLOCK * 3));
	ASSERT_TRUE(frame.owns(ptr));
	EXPECT_TRUE(frame.owns(ptr + BLOCK * 3 - 1));
	EXPECT_EQ(heap.blocks_in_use(), 4u); // three for the request, one for the alignment slack

	// The tail of the run is still usable.
	EXPECT_TRUE(frame.owns(frame.allocate_fast(16)));
	EXPECT_EQ(heap.blocks_in_use(), 4u);
}

TEST_F(Blocks, ResetGivesEveryBlockBack)
{
	void* first = frame.allocate_fast(16);
	(void)frame.allocate_fast(BLOCK);
	EXPECT_GT(heap.blocks_in_use(), 1u);

	frame.reset();

	EXPECT_EQ(heap.blocks_in_use(), 0u);
	EXPECT_GE(heap.peak(), 2 * BLOCK);
	EXPECT_EQ(frame.allocate_fast(16), first); // the same block, from the start
}

TEST_F(Blocks, RetagLeavesTheOldTagsBlocksAlone)
{
	void* first = frame.allocate_fast(16);
	ASSERT_EQ(heap.blocks_in_use(), 1u);

	frame.retag(heap_tag(1, 1));
	void* second = frame.allocate_fast(16);

	EXPECT_EQ(heap.blocks_in_use(), 2u); // the first frame's block is still out there
	EXPECT_NE(first, second);

	heap.free_blocks(TEST_TAG);
	EXPECT_EQ(heap.blocks_in_use(), 1u);
}

TEST_F(Blocks, ZeroSizedAllocationsAreUnique)
{
	void* a = frame.allocate_fast(0);
	void* b = frame.allocate_fast(0);

	EXPECT_NE(a, b);
}

TEST_F(Blocks, FeedsAPmrContainer)
{
	std::pmr::vector<int> values(&frame);
	for (int i = 0; i < 10000; ++i)
		values.push_back(i);

	EXPECT_EQ(values.back(), 9999);
	EXPECT_TRUE(frame.owns(values.data()));
}
