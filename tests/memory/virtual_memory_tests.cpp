#include <ember/core/bits.h>
#include <ember/memory/virtual_memory.h>

#include <gtest/gtest.h>

#include <cstring>

namespace vm = ember::virtual_memory;

TEST(VirtualMemory, PageInfoIsSane)
{
	EXPECT_TRUE(ember::is_power_of_two(vm::page_size()));
	EXPECT_GE(vm::allocation_granularity(), vm::page_size());
	EXPECT_EQ(vm::allocation_granularity() % vm::page_size(), 0u);
}

TEST(VirtualMemory, RoundingIsExact)
{
	const size_t page = vm::page_size();
	EXPECT_EQ(vm::round_to_page_size(0), 0u);
	EXPECT_EQ(vm::round_to_page_size(1), page);
	EXPECT_EQ(vm::round_to_page_size(page), page);
	EXPECT_EQ(vm::round_to_page_size(page + 1), 2 * page);

	const size_t granularity = vm::allocation_granularity();
	EXPECT_EQ(vm::round_to_allocation_granularity(1), granularity);
	EXPECT_EQ(vm::round_to_allocation_granularity(granularity), granularity);
}

TEST(VirtualMemory, ReserveCommitWriteRelease)
{
	const size_t size = vm::allocation_granularity();

	void* memory = vm::reserve(size);
	ASSERT_NE(memory, nullptr);
	ASSERT_TRUE(vm::commit(memory, size));

	std::memset(memory, 0xAB, size);
	EXPECT_EQ(static_cast<unsigned char*>(memory)[size - 1], 0xAB);

	EXPECT_TRUE(vm::release(memory, size));
}

TEST(VirtualMemory, CommitGrowsARegionPageByPage)
{
	const size_t page	 = vm::page_size();
	const size_t reserve = vm::round_to_allocation_granularity(4 * page);

	auto* base = static_cast<unsigned char*>(vm::reserve(reserve));
	ASSERT_NE(base, nullptr);

	for (size_t offset = 0; offset < reserve; offset += page)
	{
		ASSERT_TRUE(vm::commit(base + offset, page));
		base[offset] = 0x42; // faults if the commit lied
	}

	EXPECT_TRUE(vm::release(base, reserve));
}

TEST(VirtualMemory, DecommitThenRecommitIsWritable)
{
	const size_t size = vm::allocation_granularity();

	void* memory = vm::reserve(size);
	ASSERT_NE(memory, nullptr);
	ASSERT_TRUE(vm::commit(memory, size));
	std::memset(memory, 0xFF, size);

	ASSERT_TRUE(vm::decommit(memory, size));
	ASSERT_TRUE(vm::commit(memory, size));

	std::memset(memory, 0x11, size); // must not fault after re-commit
	EXPECT_EQ(static_cast<unsigned char*>(memory)[size - 1], 0x11);

	EXPECT_TRUE(vm::release(memory, size));
}

TEST(VirtualMemory, ReserveAndCommitIsImmediatelyUsable)
{
	const size_t size = 3 * vm::page_size();

	void* memory = vm::reserve_and_commit(size);
	ASSERT_NE(memory, nullptr);
	std::memset(memory, 0x5A, size);

	EXPECT_TRUE(vm::release(memory, vm::round_to_allocation_granularity(size)));
}

TEST(VirtualMemory, ProtectTransitions)
{
	const size_t size = vm::allocation_granularity();

	void* memory = vm::reserve(size);
	ASSERT_NE(memory, nullptr);
	ASSERT_TRUE(vm::commit(memory, size, vm::PageAccess::ReadWrite));

	auto* bytes = static_cast<unsigned char*>(memory);
	bytes[0]	= 7;

	ASSERT_TRUE(vm::protect(memory, size, vm::PageAccess::ReadOnly));
	EXPECT_EQ(bytes[0], 7); // reads remain legal on read-only pages

	ASSERT_TRUE(vm::protect(memory, size, vm::PageAccess::ReadWrite));
	bytes[0] = 9;
	EXPECT_EQ(bytes[0], 9);

	EXPECT_TRUE(vm::release(memory, size));
}
