#include <ember/core/bits.h>
#include <ember/memory/memory.h>
#include <ember/memory/memory_tracker.h>
#include <ember/memory/pmr/arena_resource.h>
#include <ember/memory/pmr/heap_resource.h>

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <memory_resource>

namespace
{
	using ember::HeapResource;
	using ember::MemoryTag;

	HeapResource& heap(MemoryTag tag = MemoryTag::Engine) { return ember::memory::heap(tag); }
}

TEST(HeapResource, AllocateGivesWritableMemory)
{
	void* ptr = heap().allocate(256, 16);
	ASSERT_NE(ptr, nullptr);

	std::memset(ptr, 0xEF, 256);
	EXPECT_EQ(static_cast<unsigned char*>(ptr)[255], 0xEF);

	heap().deallocate(ptr, 256, 16);
}

TEST(HeapResource, RespectsOverAlignment)
{
	for (size_t alignment = 16; alignment <= 4096; alignment *= 2)
	{
		void* ptr = heap().allocate(64, alignment);
		ASSERT_NE(ptr, nullptr);
		EXPECT_TRUE(ember::is_aligned(ptr, alignment)) << "alignment " << alignment;
		heap().deallocate(ptr, 64, alignment);
	}
}

TEST(HeapResource, AllocateZeroedReturnsZeroes)
{
	constexpr size_t size = 512;

	auto* bytes = static_cast<unsigned char*>(heap().allocate_zeroed(size));
	ASSERT_NE(bytes, nullptr);
	for (size_t i = 0; i < size; ++i)
		ASSERT_EQ(bytes[i], 0u) << "byte " << i;

	heap().deallocate_unsized(bytes);
}

TEST(HeapResource, ReallocatePreservesContents)
{
	auto* bytes = static_cast<unsigned char*>(heap().reallocate(nullptr, 64)); // realloc(null) == malloc
	ASSERT_NE(bytes, nullptr);
	std::memset(bytes, 0x7C, 64);

	auto* grown = static_cast<unsigned char*>(heap().reallocate(bytes, 4096));
	ASSERT_NE(grown, nullptr);
	for (size_t i = 0; i < 64; ++i)
		ASSERT_EQ(grown[i], 0x7C) << "byte " << i;

	heap().deallocate_unsized(grown);
}

TEST(HeapResource, UsableSizeCoversTheRequest)
{
	EXPECT_EQ(HeapResource::usable_size(nullptr), 0u);

	void* ptr = heap().allocate(100, 16);
	EXPECT_GE(HeapResource::usable_size(ptr), 100u);
	heap().deallocate(ptr, 100, 16);
}

TEST(HeapResource, NullFreesAreNoOps)
{
	heap().deallocate_unsized(nullptr); // must not crash
}

TEST(HeapResource, AllTaggedViewsAreOneHeap)
{
	for (size_t i = 0; i < static_cast<size_t>(MemoryTag::Count); ++i)
	{
		const auto tag = static_cast<MemoryTag>(i);
		EXPECT_EQ(heap(tag).tag(), tag);
		EXPECT_TRUE(heap(MemoryTag::Engine).is_equal(heap(tag)));
	}
}

TEST(HeapResource, CrossTagFreeIsValid)
{
	void* ptr = heap(MemoryTag::Graphics).allocate(128, 16);
	ASSERT_NE(ptr, nullptr);
	heap(MemoryTag::Audio).deallocate(ptr, 128, 16); // same underlying heap
}

TEST(HeapResource, IsNotEqualToOtherResourceKinds)
{
	ember::ArenaResource arena;
	ASSERT_TRUE(arena.init(64 * 1024, 4 * 1024));

	EXPECT_FALSE(heap().is_equal(arena));
	EXPECT_FALSE(arena.is_equal(heap()));
}

TEST(HeapResource, ServesPmrContainers)
{
	std::pmr::vector<ember::u64> values(&heap(MemoryTag::Gameplay));
	for (ember::u64 i = 0; i < 100; ++i)
		values.push_back(i);

	EXPECT_EQ(values.back(), 99u);
}

#if EMBER_MEMORY_TRACKING >= 2
TEST(HeapResourceTracking, AttributesAllocationsToTheirTag)
{
	namespace tracker = ember::memory_tracker;

	// Deltas, not absolutes: the counters are process-global and other tests run first.
	const tracker::TagStats before = tracker::stats(MemoryTag::Scripting);

	void* ptr				= heap(MemoryTag::Scripting).allocate(1000, 16);
	const size_t block_size = HeapResource::usable_size(ptr); // tracker accounts usable, not requested

	const tracker::TagStats during = tracker::stats(MemoryTag::Scripting);
	EXPECT_EQ(during.current_bytes, before.current_bytes + block_size);
	EXPECT_EQ(during.current_count, before.current_count + 1);
	EXPECT_EQ(during.total_count, before.total_count + 1);

	heap(MemoryTag::Scripting).deallocate(ptr, 1000, 16);

	const tracker::TagStats after = tracker::stats(MemoryTag::Scripting);
	EXPECT_EQ(after.current_bytes, before.current_bytes);
	EXPECT_EQ(after.current_count, before.current_count);
}

TEST(HeapResourceTracking, FreshAllocationsArePoisoned)
{
	auto* bytes = static_cast<unsigned char*>(heap().allocate(64, 16));

	for (size_t i = 0; i < 64; ++i)
		ASSERT_EQ(bytes[i], 0xCD) << "byte " << i;

	heap().deallocate(bytes, 64, 16);
}

TEST(HeapResourceTracking, LiveBlocksAreObservableAsLeaks)
{
	namespace tracker = ember::memory_tracker;

	void* block = heap(MemoryTag::Tools).allocate(64, 16);
	EXPECT_GE(tracker::report_leaks(), 1u); // logs a warning per live block — expected noise

	heap(MemoryTag::Tools).deallocate(block, 64, 16);
}
#endif

TEST(HeapResourceDeathTest, ImpossibleAllocationIsFatal)
{
	EXPECT_DEATH((void)heap().allocate(std::numeric_limits<size_t>::max() / 2, 16), "Out of memory");
}
