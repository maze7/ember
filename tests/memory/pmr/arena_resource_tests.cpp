#include <ember/core/bits.h>
#include <ember/memory/pmr/arena_resource.h>
#include <ember/memory/virtual_memory.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <vector>

namespace
{
	namespace vm = ember::virtual_memory;
	using ember::ArenaResource;
	using ember::ArenaScope;

	constexpr size_t k_reserve = 256 * 1024;
	constexpr size_t k_step	   = 4 * 1024;

	class ArenaResourceTest : public ::testing::Test
	{
	protected:
		void SetUp() override { ASSERT_TRUE(m_arena.init(k_reserve, k_step, ember::MemoryTag::Engine)); }

		ArenaResource m_arena;
	};

	// Same fixture; the DeathTest suffix makes gtest schedule these first,
	// before other tests spawn state that forking dislikes.
	using ArenaResourceDeathTest = ArenaResourceTest;
}

TEST_F(ArenaResourceTest, InitEstablishesReserveAndInitialCommit)
{
	EXPECT_EQ(m_arena.used(), 0u);
	EXPECT_EQ(m_arena.peak(), 0u);
	EXPECT_EQ(m_arena.reserved(), vm::round_to_allocation_granularity(k_reserve));
	EXPECT_EQ(m_arena.committed(), std::min(vm::round_to_page_size(k_step), m_arena.reserved()));
}

TEST_F(ArenaResourceTest, RespectsRequestedAlignment)
{
	for (size_t alignment = 1; alignment <= 256; alignment *= 2)
	{
		void* ptr = m_arena.allocate_fast(24, alignment);
		ASSERT_NE(ptr, nullptr);
		EXPECT_TRUE(ember::is_aligned(ptr, alignment)) << "alignment " << alignment;
	}
}

TEST_F(ArenaResourceTest, ZeroSizeAllocationsAreDistinct)
{
	void* a = m_arena.allocate_fast(0);
	void* b = m_arena.allocate_fast(0);
	EXPECT_NE(a, b);
}

TEST_F(ArenaResourceTest, AllocationsAreWritableAndDisjoint)
{
	auto* a = static_cast<unsigned char*>(m_arena.allocate_fast(64));
	auto* b = static_cast<unsigned char*>(m_arena.allocate_fast(64));

	std::memset(a, 0xAA, 64);
	std::memset(b, 0xBB, 64);
	EXPECT_EQ(a[63], 0xAA); // writing b must not clobber a
}

TEST_F(ArenaResourceTest, GrowsCommitOnDemandUpToReserve)
{
	const size_t before = m_arena.committed();

	(void)m_arena.allocate_fast(3 * k_step);

	EXPECT_GT(m_arena.committed(), before);
	EXPECT_GE(m_arena.committed(), m_arena.used());
	EXPECT_LE(m_arena.committed(), m_arena.reserved());
	EXPECT_EQ(m_arena.committed() % vm::page_size(), 0u);
}

TEST_F(ArenaResourceTest, MarkerResetRestoresOffsetAndKeepsPeak)
{
	(void)m_arena.allocate_fast(128);
	const ArenaResource::Marker marker = m_arena.mark();

	(void)m_arena.allocate_fast(1024);
	const size_t high_water = m_arena.used();

	m_arena.reset(marker);
	EXPECT_EQ(m_arena.used(), marker.offset);
	EXPECT_EQ(m_arena.peak(), high_water); // peak survives resets

	m_arena.reset();
	EXPECT_EQ(m_arena.used(), 0u);
}

TEST_F(ArenaResourceTest, ArenaScopeRestoresOnDestruction)
{
	(void)m_arena.allocate_fast(64);
	const size_t before = m_arena.used();

	{
		ArenaScope scope(m_arena);
		(void)m_arena.allocate_fast(4096);
		EXPECT_GT(m_arena.used(), before);
	}

	EXPECT_EQ(m_arena.used(), before);
}

TEST_F(ArenaResourceTest, OwnsExactlyItsReservation)
{
	void* inside = m_arena.allocate_fast(16);
	int on_stack = 0;

	EXPECT_TRUE(m_arena.owns(inside));
	EXPECT_FALSE(m_arena.owns(&on_stack));
	EXPECT_FALSE(m_arena.owns(nullptr));
}

TEST_F(ArenaResourceTest, DeallocateIsANoOp)
{
	void* ptr			= m_arena.allocate_fast(64);
	const size_t before = m_arena.used();

	m_arena.deallocate(ptr, 64, 16); // public memory_resource entry point

	EXPECT_EQ(m_arena.used(), before);
}

TEST_F(ArenaResourceTest, ServesPmrContainers)
{
	std::pmr::vector<int> values(&m_arena);
	for (int i = 0; i < 1000; ++i)
		values.push_back(i);

	EXPECT_TRUE(m_arena.owns(values.data()));
	EXPECT_EQ(values[999], 999);
	EXPECT_GE(m_arena.used(), 1000 * sizeof(int));
}

#if EMBER_MEMORY_TRACKING >= 2
TEST_F(ArenaResourceTest, ResetPoisonsReclaimedMemory)
{
	auto* first = static_cast<unsigned char*>(m_arena.allocate_fast(64));
	std::memset(first, 0xAB, 64);

	m_arena.reset();

	auto* second = static_cast<unsigned char*>(m_arena.allocate_fast(64));
	ASSERT_EQ(second, first); // same offset after a full reset
	for (size_t i = 0; i < 64; ++i)
		ASSERT_EQ(second[i], 0xDC) << "byte " << i;
}
#endif

TEST_F(ArenaResourceDeathTest, ExhaustingTheReserveIsFatal)
{
	EXPECT_DEATH((void)m_arena.allocate_fast(m_arena.reserved() + 1), "Out of memory");
}

TEST_F(ArenaResourceDeathTest, SizeOverflowIsFatal)
{
	EXPECT_DEATH((void)m_arena.allocate_fast(std::numeric_limits<size_t>::max()), "Out of memory");
}

#ifndef NDEBUG
TEST_F(ArenaResourceDeathTest, ResettingToAStaleMarkerTripsAnAssert)
{
	// Marker beyond the current offset is a contract violation, so debug builds trap.
	EXPECT_DEATH(m_arena.reset(ArenaResource::Marker{.offset = 128}), "assert");
}
#endif
