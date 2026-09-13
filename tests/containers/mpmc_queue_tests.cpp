#include <ember/containers/mpmc_queue.h>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace ember;

namespace
{
	struct Item
	{
		u32 producer = 0;
		u32 index	 = 0;
	};
}

TEST(MpmcQueue, FifoWithinCapacity)
{
	MpmcQueue<u32> queue(MemoryTag::Engine);
	queue.init(8);
	EXPECT_EQ(queue.capacity(), 8u);

	u32 value = 0;
	EXPECT_FALSE(queue.try_pop(value));

	for (u32 i = 0; i < 8; ++i)
		EXPECT_TRUE(queue.try_push(i));

	EXPECT_FALSE(queue.try_push(99));
	EXPECT_EQ(queue.size_hint(), 8u);

	for (u32 i = 0; i < 8; ++i)
	{
		ASSERT_TRUE(queue.try_pop(value));
		EXPECT_EQ(value, i);
	}

	EXPECT_FALSE(queue.try_pop(value));
	EXPECT_EQ(queue.size_hint(), 0u);
}

TEST(MpmcQueue, KeepsOrderAcrossLaps)
{
	MpmcQueue<u64> queue(MemoryTag::Engine);
	queue.init(4);

	u64 next_push = 0;
	u64 next_pop  = 0;

	for (int round = 0; round < 1000; ++round)
	{
		for (int k = 0; k < 3 && queue.try_push(next_push); ++k)
			++next_push;

		for (int k = 0; k < 2; ++k)
		{
			u64 value = 0;
			ASSERT_TRUE(queue.try_pop(value));
			ASSERT_EQ(value, next_pop++);
		}
	}
}

TEST(MpmcQueue, CopiesStructValues)
{
	MpmcQueue<Item> queue(MemoryTag::Engine);
	queue.init(2);

	ASSERT_TRUE(queue.try_push({.producer = 7, .index = 42}));

	Item item;
	ASSERT_TRUE(queue.try_pop(item));
	EXPECT_EQ(item.producer, 7u);
	EXPECT_EQ(item.index, 42u);
}

TEST(MpmcQueue, ManyProducersManyConsumers)
{
	constexpr u32 producers	   = 4;
	constexpr u32 consumers	   = 4;
	constexpr u32 per_producer = 100'000;
	constexpr u32 total		   = producers * per_producer;

	MpmcQueue<Item> queue(MemoryTag::Engine);
	queue.init(256);

	std::vector<std::atomic<u32>> hits(total);
	std::atomic<u32> consumed{0};
	std::atomic<bool> order_ok{true};

	std::vector<std::thread> threads;

	for (u32 p = 0; p < producers; ++p)
	{
		threads.emplace_back(
			[&, p]
			{
				for (u32 i = 0; i < per_producer; ++i)
					while (!queue.try_push({.producer = p, .index = i}))
						std::this_thread::yield();
			});
	}

	for (u32 c = 0; c < consumers; ++c)
	{
		threads.emplace_back(
			[&]
			{
				u32 last[producers];
				for (u32& l : last)
					l = 0;

				while (consumed.load(std::memory_order_relaxed) < total)
				{
					Item item;
					if (!queue.try_pop(item))
					{
						std::this_thread::yield();
						continue;
					}

					// per producer order holds within one consumer
					if (item.index + 1 < last[item.producer])
						order_ok.store(false, std::memory_order_relaxed);
					last[item.producer] = item.index + 1;

					hits[item.producer * per_producer + item.index].fetch_add(1, std::memory_order_relaxed);
					consumed.fetch_add(1, std::memory_order_relaxed);
				}
			});
	}

	for (auto& thread : threads)
		thread.join();

	EXPECT_TRUE(order_ok.load());
	EXPECT_EQ(consumed.load(), total);

	u32 missing = 0, duplicated = 0;
	for (auto& hit : hits)
	{
		const u32 count = hit.load();
		missing += count == 0;
		duplicated += count > 1;
	}

	EXPECT_EQ(missing, 0u);
	EXPECT_EQ(duplicated, 0u);
	EXPECT_EQ(queue.size_hint(), 0u);
}
