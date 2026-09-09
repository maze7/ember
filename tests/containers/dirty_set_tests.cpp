#include <ember/containers/dirty_set.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace
{
	using ember::DirtySet;
	using ember::u32;

	[[nodiscard]] std::vector<u32> marked(const DirtySet& set)
	{
		const auto slots = set.slots();
		return {slots.begin(), slots.end()};
	}

	TEST(DirtySet, MarksASlotOnceHoweverOftenItChanges)
	{
		DirtySet set(ember::MemoryTag::Engine);
		set.init(64);

		set.mark(7);
		set.mark(7);
		set.mark(7);

		EXPECT_EQ(set.size(), 1u);
		EXPECT_EQ(marked(set), (std::vector<u32>{7}));
	}

	TEST(DirtySet, KeepsTheOrderSlotsWereClaimedIn)
	{
		DirtySet set(ember::MemoryTag::Engine);
		set.init(64);

		set.mark(5);
		set.mark(1);
		set.mark(63);
		set.mark(1);

		EXPECT_EQ(marked(set), (std::vector<u32>{5, 1, 63}));
	}

	TEST(DirtySet, ClearEmptiesItAndSlotsCanBeMarkedAgain)
	{
		DirtySet set(ember::MemoryTag::Engine);
		set.init(64);

		set.mark(3);
		set.mark(40);
		set.clear();

		EXPECT_TRUE(set.empty());

		set.mark(40);

		EXPECT_EQ(marked(set), (std::vector<u32>{40}));
	}

	TEST(DirtySet, HoldsEverySlotAtCapacity)
	{
		constexpr u32 CAPACITY = 130; // not a multiple of 64, so the last word is partial

		DirtySet set(ember::MemoryTag::Engine);
		set.init(CAPACITY);

		for (u32 i = 0; i < CAPACITY; ++i)
			set.mark(i);

		EXPECT_EQ(set.size(), CAPACITY);
	}

	// The bit is the claim: whichever thread flips it appends, and the others walk away. Without
	// that, a slot two threads touch in the same frame lands in the list twice.
	TEST(DirtySet, ConcurrentMarkersAppendEachSlotExactlyOnce)
	{
		constexpr u32 THREADS = 8;
		constexpr u32 SLOTS	  = 256;

		DirtySet set(ember::MemoryTag::Engine);
		set.init(SLOTS);

		std::atomic<u32> ready = 0;
		std::vector<std::thread> pool;

		for (u32 t = 0; t < THREADS; ++t)
			pool.emplace_back(
				[&, t]
				{
					ready.fetch_add(1);
					while (ready.load() < THREADS)
						;

					// Every thread walks every slot, from its own offset, so they collide constantly.
					for (u32 i = 0; i < SLOTS; ++i)
						set.mark((i + t * 32) % SLOTS);
				});

		for (std::thread& thread : pool)
			thread.join();

		std::vector<u32> slots = marked(set);
		ASSERT_EQ(slots.size(), SLOTS);

		std::sort(slots.begin(), slots.end());
		for (u32 i = 0; i < SLOTS; ++i)
			ASSERT_EQ(slots[i], i) << "slot " << i << " was appended twice or not at all";
	}
}
