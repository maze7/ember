#include <ember/containers/ring_buffer.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
	using ember::RingBuffer;
	using ember::u32;
	using ember::u64;

	[[nodiscard]] std::vector<u32> values_of(const RingBuffer<u32, 4>& ring) { return {ring.begin(), ring.end()}; }

	TEST(RingBuffer, StartsEmpty)
	{
		RingBuffer<u32, 4> ring;

		EXPECT_TRUE(ring.empty());
		EXPECT_FALSE(ring.full());
		EXPECT_EQ(ring.size(), 0u);
		EXPECT_EQ(ring.first_sequence(), 0u);
		EXPECT_EQ(ring.next_sequence(), 0u);
		EXPECT_EQ(ring.find(0), nullptr);
		EXPECT_EQ(ring.begin(), ring.end());
	}

	TEST(RingBuffer, PushesFillTheWindowThenEvictTheOldest)
	{
		RingBuffer<u32, 4> ring;

		for (u32 i = 0; i < 3; ++i)
			ring.push(i);

		EXPECT_EQ(ring.size(), 3u);
		EXPECT_FALSE(ring.full());
		EXPECT_EQ(values_of(ring), (std::vector<u32>{0, 1, 2}));

		ring.push(3u);
		ring.push(4u);
		ring.push(5u);

		EXPECT_TRUE(ring.full());
		EXPECT_EQ(ring.size(), 4u);
		EXPECT_EQ(ring.first_sequence(), 2u);
		EXPECT_EQ(ring.next_sequence(), 6u);
		EXPECT_EQ(values_of(ring), (std::vector<u32>{2, 3, 4, 5}));
		EXPECT_EQ(ring.front(), 2u);
		EXPECT_EQ(ring.back(), 5u);
	}

	TEST(RingBuffer, ValuesAreFoundByNumberWhileInTheWindow)
	{
		RingBuffer<u32, 4> ring;

		for (u32 i = 0; i < 10; ++i)
			ring.push(i * 100);

		// Window is [6, 10).
		EXPECT_EQ(ring.find(5), nullptr);
		ASSERT_NE(ring.find(6), nullptr);
		EXPECT_EQ(*ring.find(6), 600u);
		EXPECT_EQ(*ring.find(9), 900u);
		EXPECT_EQ(ring.find(10), nullptr);

		// By age, oldest first.
		EXPECT_EQ(ring[0], 600u);
		EXPECT_EQ(ring[3], 900u);

		// The pointer is the slot itself.
		*ring.find(7) = 7u;
		EXPECT_EQ(ring[1], 7u);
	}

	TEST(RingBuffer, IterationNamesEachValuesNumber)
	{
		RingBuffer<u32, 4> ring;

		for (u32 i = 0; i < 7; ++i)
			ring.push(i);

		u64 expected = ring.first_sequence();
		for (auto it = ring.begin(); it != ring.end(); ++it, ++expected)
		{
			EXPECT_EQ(it.sequence(), expected);
			EXPECT_EQ(*it, expected);
		}

		EXPECT_EQ(expected, ring.next_sequence());
	}

	TEST(RingBuffer, PopDropsTheOldest)
	{
		RingBuffer<u32, 4> ring;

		for (u32 i = 0; i < 4; ++i)
			ring.push(i);

		ring.pop();

		EXPECT_EQ(ring.size(), 3u);
		EXPECT_EQ(ring.first_sequence(), 1u);
		EXPECT_EQ(ring.find(0), nullptr);
		EXPECT_EQ(ring.front(), 1u);
		EXPECT_EQ(values_of(ring), (std::vector<u32>{1, 2, 3}));

		// The freed room takes a push without evicting.
		ring.push(4u);
		EXPECT_EQ(values_of(ring), (std::vector<u32>{1, 2, 3, 4}));

		while (!ring.empty())
			ring.pop();

		EXPECT_EQ(ring.first_sequence(), ring.next_sequence());
		EXPECT_EQ(ring.begin(), ring.end());
	}

	TEST(RingBuffer, ClearRestartsTheNumbering)
	{
		RingBuffer<u32, 4> ring;

		for (u32 i = 0; i < 9; ++i)
			ring.push(i);

		ring.clear();

		EXPECT_TRUE(ring.empty());
		EXPECT_EQ(ring.next_sequence(), 0u);
		EXPECT_EQ(ring.find(8), nullptr);

		ring.push(42u);
		EXPECT_EQ(ring.first_sequence(), 0u);
		EXPECT_EQ(*ring.find(0), 42u);
	}

	TEST(RingBuffer, SlotsOutliveTheValuesInThem)
	{
		// A vector keeps its capacity when the slot comes round again: the ring reuses, it never
		// reconstructs.
		RingBuffer<std::vector<int>, 4> ring;

		std::vector<int>& first = ring.push();
		first.reserve(1000);
		first.push_back(7);
		int* const storage = first.data();

		for (u32 i = 0; i < 3; ++i)
			(void)ring.push();

		std::vector<int>& again = ring.push();
		EXPECT_EQ(&again, &first);
		EXPECT_GE(again.capacity(), 1000u);
		EXPECT_EQ(again.data(), storage);
		EXPECT_EQ(again.size(), 1u); // still holds what it held; the caller decides what to reset
	}

	TEST(RingBuffer, TheGeneratorBuildsEverySlotOnceInPlace)
	{
		struct Slot
		{
			int& counter;
			u32 index;

			Slot(int& c, u32 i) : counter(c), index(i) { ++counter; }
			Slot(const Slot& other) : counter(other.counter), index(other.index) { counter += 1000; }
		};

		int constructed = 0;
		RingBuffer<Slot, 4> ring([&](u32 index) { return Slot(constructed, index); });

		// Four constructions and no copies: the generator's values landed in the slots directly.
		EXPECT_EQ(constructed, 4);

		for (u32 i = 0; i < 6; ++i)
			(void)ring.push();

		EXPECT_EQ(constructed, 4);
		EXPECT_EQ(ring.front().index, 2u);
		EXPECT_EQ(ring.back().index, 1u);
	}

	TEST(RingBuffer, StorageAndOffsetDescribeTheRawArray)
	{
		RingBuffer<float, 4> ring;

		// Never wrapped: the values sit at the front of the storage.
		ring.push(1.0f);
		ring.push(2.0f);
		EXPECT_EQ(ring.storage().size(), 4u);
		EXPECT_EQ(ring.storage_offset(), 0u);

		// Full: the walk a plot makes, values[(offset + i) % count], reproduces the window.
		for (u32 i = 3; i <= 6; ++i)
			ring.push(static_cast<float>(i));

		const auto storage = ring.storage();
		std::vector<float> walked;
		for (u32 i = 0; i < ring.size(); ++i)
			walked.push_back(storage[(ring.storage_offset() + i) % ring.size()]);

		EXPECT_EQ(walked, (std::vector<float>{3.0f, 4.0f, 5.0f, 6.0f}));
		EXPECT_EQ(walked, std::vector<float>(ring.begin(), ring.end()));
	}

	TEST(RingBuffer, PushAssignsAnyAssignableValue)
	{
		RingBuffer<std::string, 2> ring;

		ring.push("one");
		ring.push(std::string("two"));

		const std::string three = "three";
		ring.push(three);

		EXPECT_EQ(ring.front(), "two");
		EXPECT_EQ(ring.back(), "three");
	}

	TEST(RingBuffer, ConstAccessReadsTheSameWindow)
	{
		RingBuffer<u32, 4> ring;
		for (u32 i = 0; i < 5; ++i)
			ring.push(i);

		const RingBuffer<u32, 4>& view = ring;

		EXPECT_EQ(view.front(), 1u);
		EXPECT_EQ(view.back(), 4u);
		EXPECT_EQ(view[2], 3u);
		EXPECT_EQ(*view.find(2), 2u);
		EXPECT_EQ(std::vector<u32>(view.begin(), view.end()), (std::vector<u32>{1, 2, 3, 4}));
	}

#if !defined(NDEBUG)
	TEST(RingBufferDeathTest, EmptyAccessAsserts)
	{
		RingBuffer<u32, 4> ring;

		EXPECT_DEATH(ring.pop(), "assert");
		EXPECT_DEATH((void)ring.front(), "assert");
		EXPECT_DEATH((void)ring.back(), "assert");
		EXPECT_DEATH((void)ring[0], "assert");
	}
#endif
}
