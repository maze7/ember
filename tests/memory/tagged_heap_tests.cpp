#include <ember/core/bits.h>
#include <ember/memory/tagged_heap.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace ember;

namespace
{
	constexpr size_t BLOCK	  = 64_kb;
	constexpr size_t CAPACITY = 16 * BLOCK;

	constexpr HeapTag TAG_A = heap_tag(1, 7);
	constexpr HeapTag TAG_B = heap_tag(2, 7);
	constexpr HeapTag TAG_C = heap_tag(3, 7);

	constexpr u32 NONE = ~u32{0};

	/// Sixteen blocks of 64 KiB: small enough that runs and fragmentation are easy to arrange.
	class BlockHeap : public ::testing::Test
	{
	protected:
		void SetUp() override { ASSERT_TRUE(heap.init(CAPACITY, BLOCK)); }

		void TearDown() override
		{
			// Every test gives back what it took; shutdown asserts on a leak in debug.
			for (const HeapTag tag : {TAG_A, TAG_B, TAG_C})
				(void)heap.free(tag);
			heap.shutdown();
		}

		TaggedHeap heap;
	};

	/// Enough blocks to span several bitlist words, which is where the scan hint has to be right.
	class WideHeap : public ::testing::Test
	{
	protected:
		static constexpr size_t SMALL = 4_kb;
		static constexpr u32 BLOCKS	  = 256;

		void SetUp() override { ASSERT_TRUE(heap.init(BLOCKS * SMALL, SMALL)); }

		void TearDown() override
		{
			for (u8 kind = 1; kind < 16; ++kind)
				(void)heap.free(heap_tag(kind));
			heap.shutdown();
		}

		TaggedHeap heap;
	};
}

TEST(HeapTag, PacksKindAndSequenceWithoutLoss)
{
	const HeapTag tag = heap_tag(3, 0x0012'3456'789A'BCDEull);

	EXPECT_EQ(kind(tag), 3u);
	EXPECT_EQ(sequence(tag), 0x0012'3456'789A'BCDEull);

	const HeapTag top = heap_tag(255, HEAP_TAG_SEQUENCE_MASK);
	EXPECT_EQ(kind(top), 255u);
	EXPECT_EQ(sequence(top), HEAP_TAG_SEQUENCE_MASK);
}

TEST(HeapTag, DistinctKindsAndFramesNeverCollide)
{
	EXPECT_NE(heap_tag(1, 7), heap_tag(2, 7));
	EXPECT_NE(heap_tag(1, 7), heap_tag(1, 8));
	EXPECT_NE(heap_tag(1, 0), NO_TAG); // kind alone keeps a tag non zero
	EXPECT_EQ(heap_tag(1, 7), heap_tag(1, 7));
}

TEST_F(BlockHeap, StartsEmptyWithTheRequestedGeometry)
{
	EXPECT_EQ(heap.block_size(), BLOCK);
	EXPECT_EQ(heap.block_count(), 16u);
	EXPECT_EQ(heap.capacity(), CAPACITY);
	EXPECT_EQ(heap.blocks_in_use(), 0u);
	EXPECT_EQ(heap.used(), 0u);
	EXPECT_EQ(heap.peak(), 0u);
	EXPECT_EQ(heap.blocks_owned(TAG_A), 0u);
}

TEST(TaggedHeapInit, RoundsCapacityUpToWholeBlocks)
{
	TaggedHeap heap;
	ASSERT_TRUE(heap.init(3 * BLOCK + 1, BLOCK));

	EXPECT_EQ(heap.capacity() % BLOCK, 0u);
	EXPECT_GE(heap.capacity(), 4 * BLOCK);
	EXPECT_EQ(heap.block_count(), heap.capacity() / BLOCK);

	heap.shutdown();
}

TEST(TaggedHeapInit, ShutdownOfAnEmptyHeapIsANoOpAndInitCanFollow)
{
	TaggedHeap heap;
	heap.shutdown(); // never initialised

	ASSERT_TRUE(heap.init(CAPACITY, BLOCK));
	heap.shutdown();
	ASSERT_TRUE(heap.init(CAPACITY, BLOCK)); // reusable after a clean shutdown
	EXPECT_EQ(heap.blocks_in_use(), 0u);
	heap.shutdown();
}

TEST_F(BlockHeap, HandsOutOneBlockUnderATag)
{
	auto* block = static_cast<u8*>(heap.allocate(1, TAG_A));

	ASSERT_NE(block, nullptr);
	EXPECT_TRUE(heap.owns(block));
	EXPECT_TRUE(is_aligned(block, 4_kb)); // blocks start on pages
	EXPECT_EQ(heap.tag_of(block), TAG_A);
	EXPECT_EQ(heap.tag_of(block + BLOCK - 1), TAG_A); // the whole block, to its last byte
	EXPECT_EQ(heap.blocks_in_use(), 1u);
	EXPECT_EQ(heap.used(), BLOCK);
	EXPECT_EQ(heap.blocks_owned(TAG_A), 1u);
	EXPECT_EQ(heap.blocks_owned(TAG_B), 0u);
}

TEST_F(BlockHeap, SingleBlocksComeOutLowestFirstAndAdjacent)
{
	auto* first	 = static_cast<u8*>(heap.allocate(1, TAG_A));
	auto* second = static_cast<u8*>(heap.allocate(1, TAG_A));
	auto* third	 = static_cast<u8*>(heap.allocate(1, TAG_B));

	ASSERT_NE(first, nullptr);
	EXPECT_EQ(second, first + BLOCK);
	EXPECT_EQ(third, first + 2 * BLOCK);
}

TEST_F(BlockHeap, ARunIsConsecutiveAndEveryBlockCarriesTheTag)
{
	auto* run = static_cast<u8*>(heap.allocate(3, TAG_A));

	ASSERT_NE(run, nullptr);
	EXPECT_EQ(heap.blocks_in_use(), 3u);
	EXPECT_EQ(heap.blocks_owned(TAG_A), 3u);

	for (size_t i = 0; i < 3; ++i)
		EXPECT_EQ(heap.tag_of(run + i * BLOCK), TAG_A) << "block " << i;

	EXPECT_EQ(heap.tag_of(run + 3 * BLOCK), NO_TAG);	 // and the one after is not ours
	EXPECT_EQ(heap.allocate(1, TAG_B), run + 3 * BLOCK); // it is the next one handed out
}

TEST_F(BlockHeap, TheWholeHeapCanBeOneRun)
{
	auto* run = static_cast<u8*>(heap.allocate(16, TAG_A));

	ASSERT_NE(run, nullptr);
	EXPECT_EQ(heap.blocks_in_use(), 16u);
	EXPECT_TRUE(heap.owns(run + CAPACITY - 1));
	EXPECT_FALSE(heap.owns(run + CAPACITY));
}

TEST_F(BlockHeap, FreeReturnsExactlyTheTagsBlocksAndReportsTheCount)
{
	void* a = heap.allocate(4, TAG_A);
	void* b = heap.allocate(4, TAG_B);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);

	EXPECT_EQ(heap.free(TAG_A), 4u);

	EXPECT_EQ(heap.blocks_in_use(), 4u);
	EXPECT_EQ(heap.blocks_owned(TAG_A), 0u);
	EXPECT_EQ(heap.blocks_owned(TAG_B), 4u);
	EXPECT_EQ(heap.tag_of(a), NO_TAG);
	EXPECT_EQ(heap.tag_of(b), TAG_B);

	EXPECT_EQ(heap.free(TAG_A), 0u); // nothing left to free
	EXPECT_EQ(heap.blocks_in_use(), 4u);
}

TEST_F(BlockHeap, FreedBlocksAreHandedOutAgainFromTheBottom)
{
	void* a = heap.allocate(4, TAG_A);
	void* b = heap.allocate(4, TAG_B);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);

	EXPECT_EQ(heap.free(TAG_A), 4u);

	// Lowest free run wins: A's old blocks, not the ones above B.
	EXPECT_EQ(heap.allocate(4, TAG_C), a);
	EXPECT_EQ(heap.allocate(1, TAG_C), static_cast<u8*>(b) + 4 * BLOCK);
}

TEST_F(BlockHeap, FreeingATagThatOwnsNothingIsANoOp)
{
	EXPECT_EQ(heap.free(TAG_A), 0u);
	EXPECT_EQ(heap.free(heap_tag(200, 123456)), 0u);
	EXPECT_EQ(heap.blocks_in_use(), 0u);
}

TEST_F(BlockHeap, InterleavedTagsAreAccountedExactly)
{
	// A B C A B C ... over the whole heap, then free the middle one.
	const HeapTag tags[] = {TAG_A, TAG_B, TAG_C};
	for (u32 i = 0; i < 16; ++i)
		ASSERT_NE(heap.allocate(1, tags[i % 3]), nullptr);

	EXPECT_EQ(heap.blocks_owned(TAG_A), 6u);
	EXPECT_EQ(heap.blocks_owned(TAG_B), 5u);
	EXPECT_EQ(heap.blocks_owned(TAG_C), 5u);

	EXPECT_EQ(heap.free(TAG_B), 5u);
	EXPECT_EQ(heap.blocks_in_use(), 11u);
	EXPECT_EQ(heap.blocks_owned(TAG_A), 6u);
	EXPECT_EQ(heap.blocks_owned(TAG_C), 5u);
}

TEST_F(BlockHeap, TagOfAForeignPointerIsNoTag)
{
	int local = 0;
	EXPECT_EQ(heap.tag_of(&local), NO_TAG);
	EXPECT_EQ(heap.tag_of(nullptr), NO_TAG);
	EXPECT_FALSE(heap.owns(&local));
	EXPECT_FALSE(heap.owns(nullptr));
}

TEST_F(BlockHeap, PeakIsTheHighWaterMarkAndSurvivesFree)
{
	ASSERT_NE(heap.allocate(5, TAG_A), nullptr);
	EXPECT_EQ(heap.peak(), 5 * BLOCK);

	ASSERT_NE(heap.allocate(2, TAG_B), nullptr);
	EXPECT_EQ(heap.peak(), 7 * BLOCK);

	EXPECT_EQ(heap.free(TAG_A), 5u);
	EXPECT_EQ(heap.blocks_in_use(), 2u);
	EXPECT_EQ(heap.peak(), 7 * BLOCK);

	ASSERT_NE(heap.allocate(1, TAG_A), nullptr);
	EXPECT_EQ(heap.peak(), 7 * BLOCK); // 3 in use, below the mark
}

#if EMBER_MEMORY_TRACKING >= 2
TEST_F(BlockHeap, PoisonsFreedBlocksInTrackingBuilds)
{
	auto* block = static_cast<u8*>(heap.allocate(2, TAG_A));
	ASSERT_NE(block, nullptr);
	block[0]			 = 0x11;
	block[2 * BLOCK - 1] = 0x22;

	EXPECT_EQ(heap.free(TAG_A), 2u);

	// Still mapped and committed, and scribbled: a stale read sees the pattern, not the data.
	EXPECT_EQ(block[0], 0xDC);
	EXPECT_EQ(block[BLOCK], 0xDC);
	EXPECT_EQ(block[2 * BLOCK - 1], 0xDC);
}
#endif

TEST_F(BlockHeap, ReportsFullInsteadOfOverAllocating)
{
	ASSERT_NE(heap.allocate(16, TAG_A), nullptr);

	EXPECT_EQ(heap.allocate(1, TAG_B), nullptr);
	EXPECT_EQ(heap.allocate(17, TAG_B), nullptr); // longer than the heap could ever be
	EXPECT_EQ(heap.blocks_owned(TAG_B), 0u);
	EXPECT_EQ(heap.blocks_in_use(), 16u);
}

// The regression this guards: a failed allocate must release the lock and return null, or
// every later call spins forever. If this test hangs, that is the bug.
TEST_F(BlockHeap, TheHeapKeepsWorkingAfterAFailedAllocation)
{
	ASSERT_NE(heap.allocate(16, TAG_A), nullptr);

	EXPECT_EQ(heap.allocate(1, TAG_B), nullptr);
	EXPECT_EQ(heap.allocate(2, TAG_B), nullptr);

	EXPECT_EQ(heap.free(TAG_A), 16u);
	EXPECT_NE(heap.allocate(1, TAG_B), nullptr); // would never return with the lock still held
	EXPECT_EQ(heap.blocks_in_use(), 1u);
}

TEST_F(BlockHeap, FragmentationOnlyBlocksRunsThatCannotFit)
{
	// Take every block, alternating tags, then give every other one back.
	for (u32 i = 0; i < 16; ++i)
		ASSERT_NE(heap.allocate(1, (i % 2 == 0) ? TAG_A : TAG_B), nullptr);

	EXPECT_EQ(heap.free(TAG_B), 8u);

	EXPECT_EQ(heap.allocate(2, TAG_B), nullptr); // no two free blocks are adjacent
	EXPECT_NE(heap.allocate(1, TAG_B), nullptr); // but singles are fine
	EXPECT_EQ(heap.blocks_in_use(), 9u);
}

TEST_F(BlockHeap, ARunReturnsToTheLowestGapThatFitsIt)
{
	// A A B B B A A A ... then free A: gaps of 2 at the bottom and 3 higher up.
	ASSERT_NE(heap.allocate(2, TAG_A), nullptr);
	auto* middle = static_cast<u8*>(heap.allocate(3, TAG_B));
	ASSERT_NE(middle, nullptr);
	ASSERT_NE(heap.allocate(3, TAG_A), nullptr);
	ASSERT_NE(heap.allocate(8, TAG_C), nullptr);
	EXPECT_EQ(heap.blocks_in_use(), 16u);

	EXPECT_EQ(heap.free(TAG_A), 5u);

	// A run of three cannot use the two block gap below B, so it lands right above B.
	EXPECT_EQ(heap.allocate(3, TAG_A), middle + 3 * BLOCK);
	// The two block gap is still there for a run of two.
	EXPECT_EQ(heap.allocate(2, TAG_A), middle - 2 * BLOCK);
}

TEST_F(WideHeap, TheHintNeverSkipsAFreeBlockBelowIt)
{
	constexpr HeapTag A = heap_tag(1);
	constexpr HeapTag B = heap_tag(2);
	constexpr HeapTag C = heap_tag(3);
	constexpr HeapTag D = heap_tag(4);

	// Fill the first bitlist word: blocks 0 to 4 under A, block 5 under B, 6 to 63 under A,
	// then one more under A so the single block hint moves past word zero.
	for (u32 i = 0; i < 5; ++i)
		ASSERT_NE(heap.allocate(1, A), nullptr);
	auto* lone = static_cast<u8*>(heap.allocate(1, B));
	ASSERT_NE(lone, nullptr);
	for (u32 i = 6; i < 65; ++i)
		ASSERT_NE(heap.allocate(1, A), nullptr);

	EXPECT_EQ(heap.blocks_in_use(), 65u);

	// Freeing B reopens block 5, below the hint; the hint has to rewind.
	EXPECT_EQ(heap.free(B), 1u);

	// A run of two cannot use block 5 and lands higher up. That scan must not raise the hint
	// past word zero, or the next single block would never find block 5.
	auto* run = static_cast<u8*>(heap.allocate(2, C));
	ASSERT_NE(run, nullptr);
	EXPECT_GT(run, lone);

	EXPECT_EQ(heap.allocate(1, D), lone);
}

TEST_F(WideHeap, FreeingHighBlocksRewindsOnlyAsFarAsNeeded)
{
	constexpr HeapTag LOW  = heap_tag(1);
	constexpr HeapTag HIGH = heap_tag(2);

	// Fill words 0 and 1 under LOW, then two blocks of word 2 under HIGH.
	for (u32 i = 0; i < 128; ++i)
		ASSERT_NE(heap.allocate(1, LOW), nullptr);
	auto* high = static_cast<u8*>(heap.allocate(1, HIGH));
	ASSERT_NE(heap.allocate(1, HIGH), nullptr);

	EXPECT_EQ(heap.free(HIGH), 2u);

	// The next single comes from the freed high blocks, not from anywhere lower, because
	// nothing lower is free. The hint rewound to word 2 and no further.
	EXPECT_EQ(heap.allocate(1, HIGH), high);
}

TEST_F(WideHeap, FillsAndDrainsEveryBlock)
{
	constexpr HeapTag A = heap_tag(1);
	const u32 count		= heap.block_count();

	std::set<void*> unique;
	auto* base = static_cast<u8*>(heap.allocate(1, A));
	ASSERT_NE(base, nullptr);
	unique.insert(base);

	for (u32 i = 1; i < count; ++i)
	{
		void* block = heap.allocate(1, A);
		ASSERT_EQ(block, base + size_t(i) * SMALL) << "block " << i; // strictly ascending
		EXPECT_TRUE(unique.insert(block).second);
	}

	EXPECT_EQ(heap.allocate(1, A), nullptr);
	EXPECT_EQ(heap.free(A), count);
	EXPECT_EQ(heap.blocks_in_use(), 0u);
	EXPECT_EQ(heap.allocate(count, A), base); // the whole heap as one run, from block zero
}

/*
 * The heap's placement rule is simple enough to restate without any of its machinery: a request
 * for n blocks gets the lowest run of n free blocks, or null. This test keeps a plain array of
 * owners, applies thousands of random allocations and frees to both, and demands that the heap
 * return exactly the block the model predicts every single time, that every count it reports
 * matches, and that the tag of every block matches on periodic full sweeps. The model has no
 * scan hint, so the hint is proven to be invisible.
 */
TEST(TaggedHeapModel, AgreesWithAReferenceModelUnderRandomTraffic)
{
	constexpr size_t SMALL = 4_kb;
	constexpr u32 BLOCKS   = 200;
	constexpr u32 TAGS	   = 6;
	constexpr u32 OPS	   = 6000;

	for (u32 seed = 1; seed <= 6; ++seed)
	{
		SCOPED_TRACE(::testing::Message() << "seed " << seed);

		TaggedHeap heap;
		ASSERT_TRUE(heap.init(BLOCKS * SMALL, SMALL));
		const u32 count = heap.block_count();

		// Block zero's address anchors the model. Lowest first puts the first block there.
		auto* base = static_cast<u8*>(heap.allocate(1, heap_tag(9)));
		ASSERT_NE(base, nullptr);
		ASSERT_EQ(heap.free(heap_tag(9)), 1u);

		std::vector<u8> owner(count, 0); // 0 free, otherwise the kind
		std::mt19937 rng(seed);

		const auto lowest_run = [&](u32 run) -> u32
		{
			u32 length = 0;
			for (u32 i = 0; i < count; ++i)
			{
				if (owner[i] != 0)
				{
					length = 0;
					continue;
				}
				if (++length == run)
					return i + 1 - run;
			}
			return NONE;
		};

		const auto owned_by = [&](u8 kind) -> u32
		{ return static_cast<u32>(std::count(owner.begin(), owner.end(), kind)); };

		u32 in_use = 0;

		for (u32 op = 0; op < OPS; ++op)
		{
			const u32 roll = rng() % 10;

			if (roll < 7)
			{
				// Mostly single blocks, sometimes a run of two to four: the talk's 99% and 1%.
				const u32 run = (rng() % 100 < 90) ? 1 : 2 + rng() % 3;
				const u8 tag  = static_cast<u8>(1 + rng() % TAGS);

				const u32 expected = lowest_run(run);
				void* ptr		   = heap.allocate(run, heap_tag(tag));

				if (expected == NONE)
				{
					ASSERT_EQ(ptr, nullptr) << "op " << op << ": the model has no run of " << run;
				}
				else
				{
					ASSERT_EQ(ptr, base + size_t(expected) * SMALL) << "op " << op << ": run of " << run;
					for (u32 i = 0; i < run; ++i)
						owner[expected + i] = tag;
					in_use += run;
				}
			}
			else
			{
				const u8 tag	   = static_cast<u8>(1 + rng() % TAGS);
				const u32 expected = owned_by(tag);

				ASSERT_EQ(heap.free(heap_tag(tag)), expected) << "op " << op << ": free of kind " << unsigned(tag);

				for (u8& o : owner)
					if (o == tag)
						o = 0;
				in_use -= expected;
			}

			ASSERT_EQ(heap.blocks_in_use(), in_use) << "op " << op;

			if (op % 200 == 0)
			{
				for (u32 i = 0; i < count; ++i)
				{
					const HeapTag expected = owner[i] != 0 ? heap_tag(owner[i]) : NO_TAG;
					ASSERT_EQ(heap.tag_of(base + size_t(i) * SMALL), expected) << "op " << op << ", block " << i;
				}
				for (u8 tag = 1; tag <= TAGS; ++tag)
					ASSERT_EQ(heap.blocks_owned(heap_tag(tag)), owned_by(tag))
						<< "op " << op << ", kind " << unsigned(tag);
			}
		}

		for (u8 tag = 1; tag <= TAGS; ++tag)
			ASSERT_EQ(heap.free(heap_tag(tag)), owned_by(tag));
		ASSERT_EQ(heap.blocks_in_use(), 0u);

		heap.shutdown();
	}
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
				{
				}

				for (u32 i = 0; i < 2; ++i)
					if (void* block = heap.allocate(1, heap_tag(3, t)))
						claimed[t].push_back(block);
			});

	for (std::thread& thread : pool)
		thread.join();

	std::set<void*> unique;
	size_t total = 0;

	for (u32 t = 0; t < THREADS; ++t)
	{
		for (void* block : claimed[t])
		{
			++total;
			EXPECT_TRUE(unique.insert(block).second) << "two threads were handed the same block";
			EXPECT_EQ(heap.tag_of(block), heap_tag(3, t));
		}
	}

	EXPECT_EQ(total, 16u); // every block went out exactly once
	EXPECT_EQ(heap.blocks_in_use(), 16u);

	for (u32 t = 0; t < THREADS; ++t)
		EXPECT_EQ(heap.free(heap_tag(3, t)), claimed[t].size());
	EXPECT_EQ(heap.blocks_in_use(), 0u);
}

/*
 * Eight threads allocate and free under their own tags on a heap too small for all of them,
 * so most iterations contend for the lock and many requests fail. Each thread stamps the blocks
 * it gets and checks the stamps before it frees them: a block handed to two owners would show
 * as a foreign stamp. Under TSan this is also the proof that the lock covers every access.
 */
TEST_F(BlockHeap, ConcurrentOwnersNeverShareABlock)
{
	constexpr u32 THREADS	 = 8;
	constexpr u32 ITERATIONS = 400;

	std::atomic<u32> ready	  = 0;
	std::atomic<u32> failures = 0;
	std::vector<std::thread> pool;

	for (u32 t = 0; t < THREADS; ++t)
		pool.emplace_back(
			[&, t]
			{
				const HeapTag tag = heap_tag(4, t);
				const u8 stamp	  = static_cast<u8>(0x10 + t);
				std::vector<std::pair<u8*, u32>> mine;

				ready.fetch_add(1);
				while (ready.load() < THREADS)
				{
				}

				for (u32 i = 0; i < ITERATIONS; ++i)
				{
					const u32 run = 1 + (i % 3);
					if (auto* base = static_cast<u8*>(heap.allocate(run, tag)))
					{
						for (u32 b = 0; b < run; ++b)
						{
							if (heap.tag_of(base + b * BLOCK) != tag)
								failures.fetch_add(1);
							base[b * BLOCK]				= stamp; // first and last byte of every block
							base[b * BLOCK + BLOCK - 1] = stamp;
						}
						mine.emplace_back(base, run);
					}

					if (i % 8 == 7)
					{
						u32 expected = 0;
						for (const auto& [base, run] : mine)
						{
							expected += run;
							for (u32 b = 0; b < run; ++b)
								if (base[b * BLOCK] != stamp || base[b * BLOCK + BLOCK - 1] != stamp)
									failures.fetch_add(1);
						}

						if (heap.free(tag) != expected)
							failures.fetch_add(1);
						mine.clear();
					}
				}

				(void)heap.free(tag);
			});

	for (std::thread& thread : pool)
		thread.join();

	EXPECT_EQ(failures.load(), 0u);
	EXPECT_EQ(heap.blocks_in_use(), 0u);
	for (u32 t = 0; t < THREADS; ++t)
		EXPECT_EQ(heap.blocks_owned(heap_tag(4, t)), 0u);
}

#ifndef NDEBUG
using BlockHeapDeathTest = BlockHeap;

TEST_F(BlockHeapDeathTest, ShuttingDownWithBlocksOwnedAsserts)
{
	ASSERT_NE(heap.allocate(1, TAG_A), nullptr);
	EXPECT_DEATH(heap.shutdown(), "still owns");
}

TEST_F(BlockHeapDeathTest, AllocatingUnderNoTagAsserts) { EXPECT_DEATH((void)heap.allocate(1, NO_TAG), "assert"); }

TEST_F(BlockHeapDeathTest, AllocatingZeroBlocksAsserts) { EXPECT_DEATH((void)heap.allocate(0, TAG_A), "assert"); }

TEST_F(BlockHeapDeathTest, FreeingNoTagAsserts) { EXPECT_DEATH((void)heap.free(NO_TAG), "assert"); }

TEST_F(BlockHeapDeathTest, InitialisingTwiceAsserts) { EXPECT_DEATH((void)heap.init(CAPACITY, BLOCK), "assert"); }

TEST(TaggedHeapDeathTest, ABlockSizeThatIsNotAPowerOfTwoAsserts)
{
	TaggedHeap heap;
	EXPECT_DEATH((void)heap.init(CAPACITY, 3 * 4_kb), "assert");
}

TEST(TaggedHeapDeathTest, ATagWithKindZeroAsserts)
{
	// Kind zero would make heap_tag(0, 0) equal NO_TAG. The volatile keeps the call at runtime.
	volatile u8 zero = 0;
	EXPECT_DEATH((void)heap_tag(zero, 0), "assert");
}
#endif
