#include <ember/core/bits.h>
#include <ember/memory/pmr/arena.h>
#include <ember/memory/tagged_heap.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory_resource>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace ember;

namespace
{
	// Small enough that a handful of allocations cross blocks, big enough to stay realistic.
	constexpr size_t BLOCK	  = 64_kb;
	constexpr size_t CAPACITY = 16 * BLOCK;

	constexpr HeapTag FRAME_1 = heap_tag(1, 1);
	constexpr HeapTag FRAME_2 = heap_tag(1, 2);
	constexpr HeapTag OTHER	  = heap_tag(2, 1);

	class ArenaTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			ASSERT_TRUE(heap.init(CAPACITY, BLOCK));
			arena.init(heap, "test");
			arena.begin(FRAME_1);
		}

		void TearDown() override
		{
			arena.shutdown(); // frees whatever tag it holds
			for (const HeapTag tag : {FRAME_1, FRAME_2, OTHER})
				(void)heap.free(tag);
			heap.shutdown();
		}

		/// Every byte of [ptr, ptr + size) lies in a block owned by the arena's current tag.
		[[nodiscard]] bool fully_owned(const void* ptr, size_t size) const
		{
			const u8* p = static_cast<const u8*>(ptr);
			return arena.owns(p) && arena.owns(p + (size == 0 ? 1 : size) - 1);
		}

		TaggedHeap heap;
		Arena arena;
	};

	/// Runs fn on a fresh OS thread and waits for it.
	template <class Fn> void on_a_thread(Fn&& fn)
	{
		std::thread thread(static_cast<Fn&&>(fn));
		thread.join();
	}
}

TEST_F(ArenaTest, HandsOutMemoryFromABlockOwnedByItsTag)
{
	EXPECT_EQ(heap.blocks_in_use(), 0u); // nothing is taken before the first allocation

	void* ptr = arena.allocate_fast(16);

	ASSERT_NE(ptr, nullptr);
	EXPECT_TRUE(fully_owned(ptr, 16));
	EXPECT_EQ(heap.tag_of(ptr), FRAME_1);
	EXPECT_EQ(heap.blocks_in_use(), 1u);
	EXPECT_EQ(heap.blocks_owned(FRAME_1), 1u);
}

TEST_F(ArenaTest, BumpsInsideOneBlockWithoutGoingBackToTheHeap)
{
	auto* first = static_cast<u8*>(arena.allocate_fast(16));
	ASSERT_NE(first, nullptr);

	u8* previous = first;
	for (int i = 0; i < 100; ++i)
	{
		auto* next = static_cast<u8*>(arena.allocate_fast(256));
		EXPECT_GE(next, previous + 16); // strictly after the previous allocation
		EXPECT_TRUE(fully_owned(next, 256));
		previous = next;
	}

	EXPECT_EQ(heap.blocks_in_use(), 1u);
	EXPECT_EQ(arena.stats().refills, 1u);
}

TEST_F(ArenaTest, AllocationsNeverOverlapAndHonourEveryAlignment)
{
	struct Piece
	{
		u8* begin;
		size_t size;
	};

	std::vector<Piece> pieces;
	std::mt19937 rng(42);
	size_t requested = 0;

	for (int i = 0; i < 2000; ++i)
	{
		const size_t size	   = rng() % 700;			   // zero included
		const size_t alignment = size_t{1} << (rng() % 9); // 1 to 256

		auto* ptr = static_cast<u8*>(arena.allocate_fast(size, alignment));
		ASSERT_NE(ptr, nullptr);
		ASSERT_TRUE(is_aligned(ptr, alignment)) << "allocation " << i << " alignment " << alignment;
		ASSERT_TRUE(fully_owned(ptr, size)) << "allocation " << i;

		pieces.push_back({ptr, size == 0 ? 1 : size});
		requested += size == 0 ? 1 : size;
	}

	std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.begin < b.begin; });
	for (size_t i = 1; i < pieces.size(); ++i)
		ASSERT_GE(pieces[i].begin, pieces[i - 1].begin + pieces[i - 1].size)
			<< "piece " << i << " overlaps its neighbour";

	const Arena::Stats stats = arena.stats();
	EXPECT_EQ(stats.bytes_requested, requested);
	EXPECT_EQ(stats.blocks, heap.blocks_owned(FRAME_1));
	EXPECT_GE(stats.bytes_owned, stats.bytes_requested); // padding and tails only ever add
}

TEST_F(ArenaTest, LargeAlignmentsAreHonouredAcrossARefill)
{
	(void)arena.allocate_fast(1); // leave the cursor odd
	for (size_t alignment : {size_t{16}, size_t{64}, size_t{256}, size_t{4096}})
	{
		void* ptr = arena.allocate_fast(32, alignment);
		EXPECT_TRUE(is_aligned(ptr, alignment)) << "alignment " << alignment;
		EXPECT_TRUE(fully_owned(ptr, 32));
	}

	// Force a refill with a page aligned request that does not fit the tail of the block.
	(void)arena.allocate_fast(BLOCK - 8192);
	void* ptr = arena.allocate_fast(4096, 4096);
	EXPECT_TRUE(is_aligned(ptr, 4096));
	EXPECT_TRUE(fully_owned(ptr, 4096));
	EXPECT_EQ(heap.blocks_in_use(), 2u);
}

TEST_F(ArenaTest, ZeroSizedAllocationsAreUnique)
{
	void* a = arena.allocate_fast(0);
	void* b = arena.allocate_fast(0);
	void* c = arena.allocate_fast(0, 64);

	EXPECT_NE(a, b);
	EXPECT_NE(b, c);
	EXPECT_TRUE(is_aligned(c, 64));
}

TEST_F(ArenaTest, TakesANewBlockExactlyWhenTheCurrentOneCannotFit)
{
	auto* first = static_cast<u8*>(arena.allocate_fast(BLOCK / 2));
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(heap.blocks_in_use(), 1u);

	// Fits with nothing to spare: the whole block is usable.
	auto* rest = static_cast<u8*>(arena.allocate_fast(BLOCK / 2));
	EXPECT_EQ(rest, first + BLOCK / 2);
	EXPECT_EQ(heap.blocks_in_use(), 1u);

	// One more byte and it is a new block.
	auto* next = static_cast<u8*>(arena.allocate_fast(1));
	EXPECT_EQ(heap.blocks_in_use(), 2u);
	EXPECT_EQ(heap.tag_of(next), FRAME_1);
	EXPECT_EQ(arena.stats().refills, 2u);
}

TEST_F(ArenaTest, AnOversizeAllocationSpansARunAndItsTailIsReused)
{
	auto* ptr = static_cast<u8*>(arena.allocate_fast(BLOCK * 3));
	ASSERT_NE(ptr, nullptr);
	EXPECT_TRUE(fully_owned(ptr, BLOCK * 3));

	// Three blocks for the request plus one for the alignment slack the arena always asks for.
	EXPECT_EQ(heap.blocks_in_use(), 4u);
	EXPECT_EQ(arena.stats().blocks, 4u);

	// The tail of the run serves the next request without another refill.
	auto* tail = static_cast<u8*>(arena.allocate_fast(16));
	EXPECT_EQ(tail, ptr + BLOCK * 3);
	EXPECT_EQ(heap.blocks_in_use(), 4u);
	EXPECT_EQ(arena.stats().refills, 1u);
}

TEST_F(ArenaTest, AnAllocationOfExactlyOneBlockWorks)
{
	auto* ptr = static_cast<u8*>(arena.allocate_fast(BLOCK));
	ASSERT_NE(ptr, nullptr);
	EXPECT_TRUE(fully_owned(ptr, BLOCK));
	EXPECT_LE(heap.blocks_in_use(), 2u); // one block plus at most one of slack
}

TEST_F(ArenaTest, TheAbandonedTailIsAccountedAsOwnedNotRequested)
{
	(void)arena.allocate_fast(BLOCK - 100); // leaves 100 bytes
	(void)arena.allocate_fast(200);			// does not fit: refill, the 100 are lost

	const Arena::Stats stats = arena.stats();
	EXPECT_EQ(stats.bytes_requested, BLOCK + 100);
	EXPECT_EQ(stats.blocks, 2u);
	EXPECT_EQ(stats.bytes_owned, 2 * BLOCK);
	EXPECT_EQ(stats.refills, 2u);
	EXPECT_EQ(stats.threads, 1u);
}

TEST(ArenaLifecycle, StartsInactiveAndUnbound)
{
	Arena arena;
	EXPECT_FALSE(arena.is_active());
	EXPECT_EQ(arena.tag(), NO_TAG);
	EXPECT_FALSE(arena.owns(&arena));
	EXPECT_EQ(arena.stats().refills, 0u);
	arena.shutdown(); // harmless when never bound
}

TEST_F(ArenaTest, BeginActivatesAndEndReturnsTheTag)
{
	EXPECT_TRUE(arena.is_active());
	EXPECT_EQ(arena.tag(), FRAME_1);
	EXPECT_STREQ(arena.name(), "test");

	EXPECT_EQ(arena.end(), FRAME_1);
	EXPECT_FALSE(arena.is_active());
	EXPECT_EQ(arena.tag(), FRAME_1); // still names the last frame, for the consumer's free

	arena.begin(FRAME_2);
	EXPECT_TRUE(arena.is_active());
	EXPECT_EQ(arena.tag(), FRAME_2);
}

TEST_F(ArenaTest, EndThenFreeGivesEveryBlockBack)
{
	void* first = arena.allocate_fast(16);
	(void)arena.allocate_fast(BLOCK);
	const u32 owned = heap.blocks_in_use();
	EXPECT_GT(owned, 1u);

	EXPECT_EQ(heap.free(arena.end()), owned);
	EXPECT_EQ(heap.blocks_in_use(), 0u);
	EXPECT_FALSE(arena.owns(first)); // the block is nobody's now

	arena.begin(FRAME_2);
	EXPECT_EQ(arena.allocate_fast(16), first); // the same block, from the start
}

TEST_F(ArenaTest, BeginForgetsTheOldCursorsSoTheNewTagStartsInItsOwnBlock)
{
	auto* old = static_cast<u8*>(arena.allocate_fast(16)); // block one, mostly empty
	(void)arena.end();
	arena.begin(FRAME_2);

	auto* fresh = static_cast<u8*>(arena.allocate_fast(16));

	// Not the rest of frame one's block: that memory is frame one's until its consumer frees it.
	EXPECT_EQ(heap.tag_of(fresh), FRAME_2);
	EXPECT_EQ(heap.tag_of(old), FRAME_1);
	EXPECT_EQ(heap.blocks_in_use(), 2u);
	EXPECT_EQ(heap.blocks_owned(FRAME_1), 1u);
	EXPECT_EQ(heap.blocks_owned(FRAME_2), 1u);
}

// The game to render shape: the arena moves on to the next frame while the previous frame's
// memory stays alive and intact until its consumer frees it.
TEST_F(ArenaTest, MovingToTheNextTagLeavesTheOldMemoryAliveAndIntact)
{
	auto* old = static_cast<u8*>(arena.allocate_fast(4096));
	for (size_t i = 0; i < 4096; ++i)
		old[i] = static_cast<u8>(i);

	const HeapTag previous = arena.end();
	arena.begin(FRAME_2);

	// Fill frame two hard, into several blocks, while frame one is still alive.
	for (int i = 0; i < 40; ++i)
		std::memset(arena.allocate_fast(BLOCK / 4), 0xAB, BLOCK / 4);

	for (size_t i = 0; i < 4096; ++i)
		ASSERT_EQ(old[i], static_cast<u8>(i)) << "byte " << i << " of frame one was overwritten";

	EXPECT_EQ(heap.tag_of(old), previous);
	EXPECT_FALSE(arena.owns(old)); // no longer the arena's, still the heap's
	EXPECT_EQ(heap.free(previous), 1u);
	EXPECT_EQ(heap.tag_of(old), NO_TAG);
}

TEST_F(ArenaTest, ShutdownFreesTheTagItHoldsEvenWithoutEnd)
{
	(void)arena.allocate_fast(16);
	EXPECT_EQ(heap.blocks_in_use(), 1u);

	arena.shutdown();

	EXPECT_EQ(heap.blocks_in_use(), 0u);
	EXPECT_FALSE(arena.is_active());
	EXPECT_EQ(arena.tag(), NO_TAG);

	// Reusable: bind it again.
	arena.init(heap, "again");
	arena.begin(FRAME_2);
	EXPECT_NE(arena.allocate_fast(16), nullptr);
}

TEST_F(ArenaTest, ShutdownAfterEndFreesTheEndedTag)
{
	(void)arena.allocate_fast(16);
	(void)arena.end();

	arena.shutdown();
	EXPECT_EQ(heap.blocks_in_use(), 0u);
}

TEST_F(ArenaTest, StatsSurviveEndAndResetOnBegin)
{
	(void)arena.allocate_fast(100);
	(void)arena.allocate_fast(BLOCK - 64); // does not fit behind the first: a second block

	Arena::Stats stats = arena.stats();
	EXPECT_EQ(stats.bytes_requested, 100 + BLOCK - 64);
	EXPECT_EQ(stats.blocks, 2u);
	EXPECT_EQ(stats.bytes_owned, 2 * BLOCK);
	EXPECT_EQ(stats.refills, 2u);
	EXPECT_EQ(stats.threads, 1u);

	(void)arena.end();
	stats = arena.stats();
	EXPECT_EQ(stats.refills, 2u); // still readable after the stage joined

	arena.begin(FRAME_2);
	stats = arena.stats();
	EXPECT_EQ(stats.refills, 0u);
	EXPECT_EQ(stats.bytes_requested, 0u);
	EXPECT_EQ(stats.threads, 0u);
}

TEST_F(ArenaTest, TwoArenasShareTheHeapWithoutMixingTags)
{
	Arena other;
	other.init(heap, "other");
	other.begin(OTHER);

	void* mine	 = arena.allocate_fast(16);
	void* theirs = other.allocate_fast(16);

	EXPECT_TRUE(arena.owns(mine));
	EXPECT_FALSE(arena.owns(theirs));
	EXPECT_TRUE(other.owns(theirs));
	EXPECT_FALSE(other.owns(mine));
	EXPECT_EQ(heap.blocks_in_use(), 2u);

	EXPECT_EQ(heap.free(other.end()), 1u);
	EXPECT_EQ(heap.blocks_in_use(), 1u);
	EXPECT_TRUE(arena.owns(mine));

	other.shutdown();
}

TEST_F(ArenaTest, FeedsPmrContainers)
{
	std::pmr::vector<int> values(&arena);
	for (int i = 0; i < 10000; ++i)
		values.push_back(i);

	EXPECT_EQ(values.back(), 9999);
	EXPECT_TRUE(arena.owns(values.data()));

	std::pmr::string text(&arena);
	for (int i = 0; i < 1000; ++i)
		text += "abc";
	EXPECT_EQ(text.size(), 3000u);
	EXPECT_TRUE(arena.owns(text.data()));

	HashMap<int, int> map(&arena);
	for (int i = 0; i < 1000; ++i)
		map[i] = i * i;
	EXPECT_EQ(map[31], 961);
}

TEST_F(ArenaTest, TheMemoryResourceInterfaceHonoursAlignment)
{
	std::pmr::memory_resource& resource = arena;

	void* ptr = resource.allocate(24, 64);
	EXPECT_TRUE(is_aligned(ptr, 64));
	EXPECT_TRUE(fully_owned(ptr, 24));

	resource.deallocate(ptr, 24, 64); // accepted and ignored
	EXPECT_EQ(heap.blocks_in_use(), 1u);
}

TEST_F(ArenaTest, EqualityIsIdentity)
{
	Arena other;
	other.init(heap, "other");

	std::pmr::memory_resource& a = arena;
	std::pmr::memory_resource& b = other;

	EXPECT_TRUE(a.is_equal(a));
	EXPECT_FALSE(a.is_equal(b));
	EXPECT_FALSE(a.is_equal(*std::pmr::new_delete_resource()));

	other.shutdown();
}

TEST_F(ArenaTest, ConsumersMayDestroyContainersAfterEndWhileTheTagLives)
{
	auto* values = new std::pmr::vector<int>(&arena);
	values->assign(100, 7);

	const HeapTag tag = arena.end();

	// The stage joined, the memory is still the tag's: freeing through the resource is legal.
	delete values;

	EXPECT_EQ(heap.free(tag), 1u);
}

TEST(ArenaSlots, TheMainThreadHoldsASlotAndAFreshThreadDoesNot)
{
	// tests/main.cpp registered this thread through memory::initialize().
	EXPECT_NE(Arena::thread_slot(), Arena::NO_SLOT);
	EXPECT_LT(Arena::thread_slot(), Arena::MAX_THREADS);

	u32 fresh = 0;
	on_a_thread([&] { fresh = Arena::thread_slot(); });
	EXPECT_EQ(fresh, Arena::NO_SLOT);
}

TEST(ArenaSlots, RegisterClaimsASlotAndUnregisterReleasesExactlyThatSlot)
{
	const u32 main_slot = Arena::thread_slot();
	u32 first = Arena::NO_SLOT, after = Arena::NO_SLOT, second = Arena::NO_SLOT;
	bool registered = false;

	on_a_thread(
		[&]
		{
			registered = Arena::register_thread();
			first	   = Arena::thread_slot();
			Arena::unregister_thread();
			after = Arena::thread_slot();
		});

	EXPECT_TRUE(registered);
	EXPECT_NE(first, Arena::NO_SLOT);
	EXPECT_NE(first, main_slot);
	EXPECT_EQ(after, Arena::NO_SLOT);

	// The released slot is the lowest free one again, and main's slot was never disturbed.
	on_a_thread(
		[&]
		{
			(void)Arena::register_thread();
			second = Arena::thread_slot();
			Arena::unregister_thread();
		});

	EXPECT_EQ(second, first);
	EXPECT_EQ(Arena::thread_slot(), main_slot);
}

TEST(ArenaSlots, UnregisteringAThreadThatNeverRegisteredIsANoOp)
{
	const u32 main_slot = Arena::thread_slot();

	on_a_thread([] { Arena::unregister_thread(); });

	// A stray unregister must not free somebody else's slot.
	u32 next = Arena::NO_SLOT;
	on_a_thread(
		[&]
		{
			(void)Arena::register_thread();
			next = Arena::thread_slot();
			Arena::unregister_thread();
		});

	EXPECT_NE(next, main_slot);
	EXPECT_EQ(Arena::thread_slot(), main_slot);
}

TEST(ArenaSlots, EverySlotCanBeHeldAtOnceAndOneMoreIsRefused)
{
	const u32 main_slot = Arena::thread_slot();
	const u32 extra		= Arena::MAX_THREADS - 1; // main holds one

	std::atomic<u32> registered{0};
	std::atomic<bool> release{false};
	std::vector<u32> slots(extra, Arena::NO_SLOT);
	std::vector<std::thread> holders;

	for (u32 i = 0; i < extra; ++i)
		holders.emplace_back(
			[&, i]
			{
				if (Arena::register_thread())
				{
					slots[i] = Arena::thread_slot();
					registered.fetch_add(1);
				}
				while (!release.load())
				{
				}
				Arena::unregister_thread();
			});

	while (registered.load() < extra)
	{
	}

	// All distinct, none is main's.
	std::vector<u32> sorted = slots;
	std::sort(sorted.begin(), sorted.end());
	EXPECT_EQ(std::unique(sorted.begin(), sorted.end()), sorted.end());
	EXPECT_EQ(std::count(sorted.begin(), sorted.end(), main_slot), 0);
	EXPECT_EQ(std::count(sorted.begin(), sorted.end(), Arena::NO_SLOT), 0);

	// The sixty fifth thread is refused rather than handed a slot in use.
	bool refused = false;
	on_a_thread(
		[&]
		{
			refused = !Arena::register_thread();
			if (!refused)
				Arena::unregister_thread();
		});
	EXPECT_TRUE(refused);

	release.store(true);
	for (std::thread& holder : holders)
		holder.join();

	// Everything came back: a new thread registers, and main still has its slot.
	bool again = false;
	on_a_thread(
		[&]
		{
			again = Arena::register_thread();
			Arena::unregister_thread();
		});
	EXPECT_TRUE(again);
	EXPECT_EQ(Arena::thread_slot(), main_slot);
}

TEST_F(ArenaTest, EachRegisteredThreadBumpsItsOwnBlock)
{
	constexpr u32 THREADS = 4;

	std::atomic<u32> ready{0};
	std::vector<std::thread> pool;
	std::vector<u8*> firsts(THREADS, nullptr);

	for (u32 t = 0; t < THREADS; ++t)
		pool.emplace_back(
			[&, t]
			{
				ASSERT_TRUE(Arena::register_thread());
				ready.fetch_add(1);
				while (ready.load() < THREADS)
				{
				}

				firsts[t] = static_cast<u8*>(arena.allocate_fast(64));
				for (int i = 0; i < 100; ++i)
					(void)arena.allocate_fast(64);

				Arena::unregister_thread();
			});

	for (std::thread& thread : pool)
		thread.join();

	// One block per thread, all under the arena's tag, all different.
	std::vector<u8*> sorted = firsts;
	std::sort(sorted.begin(), sorted.end());
	EXPECT_EQ(std::unique(sorted.begin(), sorted.end()), sorted.end());
	for (u8* first : firsts)
		EXPECT_EQ(heap.tag_of(first), FRAME_1);

	EXPECT_EQ(heap.blocks_in_use(), THREADS);
	EXPECT_EQ(arena.stats().threads, THREADS);
	EXPECT_EQ(arena.stats().refills, THREADS);
}

using ArenaDeathTest = ArenaTest;

// Out of memory is fatal in every build, not just debug: the process must die, not hand out null.
TEST_F(ArenaDeathTest, ARequestLargerThanTheHeapIsFatal)
{
	EXPECT_DEATH((void)arena.allocate_fast(CAPACITY * 2), "Out of memory");
}

TEST_F(ArenaDeathTest, AnExhaustedHeapIsFatal)
{
	ASSERT_NE(heap.allocate(16, OTHER), nullptr); // somebody else has every block
	EXPECT_DEATH((void)arena.allocate_fast(16), "Out of memory");
}

#ifndef NDEBUG
TEST_F(ArenaDeathTest, AllocatingAfterEndAsserts)
{
	(void)arena.end();
	EXPECT_DEATH((void)arena.allocate_fast(16), "between end");
}

TEST_F(ArenaDeathTest, BeginningTwiceAsserts) { EXPECT_DEATH(arena.begin(FRAME_2), "not ended"); }

TEST_F(ArenaDeathTest, EndingAnInactiveArenaAsserts)
{
	(void)arena.end();
	EXPECT_DEATH((void)arena.end(), "not begun");
}

TEST_F(ArenaDeathTest, BeginningWithNoTagAsserts)
{
	(void)arena.end();
	EXPECT_DEATH(arena.begin(NO_TAG), "assert");
}

TEST(ArenaLifecycleDeathTest, BeginningBeforeInitAsserts)
{
	Arena arena;
	EXPECT_DEATH(arena.begin(FRAME_1), "before init");
}

TEST_F(ArenaDeathTest, InitialisingTwiceAsserts) { EXPECT_DEATH(arena.init(heap, "twice"), "init runs once"); }

TEST_F(ArenaDeathTest, AContainerThatOutlivesItsFrameIsCaughtOnFree)
{
	// The vector holds the resource pointer; by the time it deallocates the arena has moved on.
	auto* values = new std::pmr::vector<int>(&arena);
	values->push_back(1);

	(void)heap.free(arena.end());
	arena.begin(FRAME_2);

	EXPECT_DEATH(delete values, "current tag");
}

TEST_F(ArenaDeathTest, FreeingForeignMemoryThroughTheArenaAsserts)
{
	int local							= 0;
	std::pmr::memory_resource& resource = arena;
	EXPECT_DEATH(resource.deallocate(&local, sizeof(local), alignof(int)), "current tag");
}

TEST_F(ArenaDeathTest, AnUnregisteredThreadCannotAllocate)
{
	EXPECT_DEATH(on_a_thread([&] { (void)arena.allocate_fast(16); }), "registered");
}

TEST(ArenaSlotsDeathTest, RegisteringTwiceOnOneThreadAsserts)
{
	EXPECT_DEATH(on_a_thread(
					 []
					 {
						 (void)Arena::register_thread();
						 (void)Arena::register_thread();
					 }),
				 "already holds");
}
#endif
