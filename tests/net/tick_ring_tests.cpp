#include <ember/net/tick.h>

#include <gtest/gtest.h>

namespace
{
	using namespace ember;
	using namespace ember::net;

	TEST(TickRing, FindsWhatWasWrittenAndMissesWhatWasEvicted)
	{
		TickRing<u32, 8> ring;

		ring.write(3) = 30;
		ring.write(5, 50u);

		ASSERT_NE(ring.find(3), nullptr);
		EXPECT_EQ(*ring.find(3), 30u);
		EXPECT_EQ(*ring.find(5), 50u);
		EXPECT_FALSE(ring.contains(4)) << "never written";
		EXPECT_FALSE(ring.contains(11)) << "the slot of 3, but not tick 11";

		ring.write(11) = 110; // claims 3's slot
		EXPECT_FALSE(ring.contains(3));
		EXPECT_EQ(*ring.find(11), 110u);
	}

	TEST(TickRing, ForgetsOnRequest)
	{
		TickRing<u32, 8> ring;
		ring.write(1) = 1;
		ring.write(2) = 2;

		ring.erase(9); // shares 1's slot: not 1, so nothing happens
		EXPECT_TRUE(ring.contains(1));

		ring.erase(1);
		EXPECT_FALSE(ring.contains(1));
		EXPECT_TRUE(ring.contains(2));

		ring.clear();
		EXPECT_FALSE(ring.contains(2));
	}

	TEST(TickRing, NoTickIsNeverFound)
	{
		const TickRing<u32, 8> ring;
		EXPECT_EQ(ring.find(NO_TICK), nullptr);
		EXPECT_FALSE(ring.contains(8)) << "an empty slot holds no tick, not tick 0's neighbour";
	}
}
