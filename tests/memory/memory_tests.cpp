#include <ember/core/bits.h>
#include <ember/memory/memory.h>
#include <ember/memory/memory_tracker.h>
#include <ember/memory/pmr/arena_resource.h>

#include <gtest/gtest.h>

#include <memory_resource>

namespace
{
	using ember::MemoryTag;

	struct Widget
	{
		static inline int live = 0;

		explicit Widget(int v) : value(v) { ++live; }
		~Widget() { --live; }

		int value;
	};

	struct alignas(64) OverAligned
	{
		float data[4] = {};
	};
}

TEST(Memory, DefaultPmrResourceIsTheEngineHeap)
{
	std::pmr::memory_resource* resource = std::pmr::get_default_resource();
	EXPECT_TRUE(resource->is_equal(ember::memory::heap(MemoryTag::Unknown)));
}

TEST(Memory, FrameArenaIsUsableAndScoped)
{
	ember::ArenaResource& arena = ember::memory::frame_arena();
	const size_t before			= arena.used();

	{
		ember::ArenaScope scope(arena);
		void* ptr = arena.allocate_fast(256);
		EXPECT_TRUE(arena.owns(ptr));
	}

	EXPECT_EQ(arena.used(), before);
}

// This test instantiates delete_object for the first time anywhere in the
// codebase, it is what surfaces the ptr->T() / ptr->~T() bug at compile time.
TEST(Memory, NewObjectConstructsAndDeleteObjectDestroys)
{
	auto& engine_heap = ember::memory::heap(MemoryTag::Engine);

	Widget* widget = ember::memory::new_object<Widget>(engine_heap, 42);
	ASSERT_NE(widget, nullptr);
	EXPECT_EQ(widget->value, 42);
	EXPECT_EQ(Widget::live, 1);

	ember::memory::delete_object(engine_heap, widget);
	EXPECT_EQ(Widget::live, 0); // destructor must actually have run
}

TEST(Memory, DeleteObjectOnNullIsANoOp)
{
	ember::memory::delete_object<Widget>(ember::memory::heap(), nullptr);
	EXPECT_EQ(Widget::live, 0);
}

TEST(Memory, NewObjectHonorsAlignment)
{
	auto& engine_heap = ember::memory::heap(MemoryTag::Engine);

	OverAligned* object = ember::memory::new_object<OverAligned>(engine_heap);
	EXPECT_TRUE(ember::is_aligned(object, alignof(OverAligned)));
	ember::memory::delete_object(engine_heap, object);
}

#if EMBER_MEMORY_TRACKING >= 1
TEST(Memory, TrackerTotalsFollowHeapTraffic)
{
	namespace tracker = ember::memory_tracker;

	const tracker::TagStats before = tracker::total();

	void* ptr					   = ember::memory::heap(MemoryTag::Engine).allocate(2048, 16);
	const tracker::TagStats during = tracker::total();
	EXPECT_GT(during.current_bytes, before.current_bytes);
	EXPECT_GT(during.total_count, before.total_count);

	ember::memory::heap(MemoryTag::Engine).deallocate(ptr, 2048, 16);
	const tracker::TagStats after = tracker::total();
	EXPECT_EQ(after.current_bytes, before.current_bytes);
}
#endif

TEST(MemoryDeathTest, OutOfMemoryIsFatal)
{
	EXPECT_DEATH(ember::out_of_memory(1024, 16, MemoryTag::Engine), "Out of memory");
}

#ifndef NDEBUG
TEST(MemoryDeathTest, HeapLookupWithBogusTagTripsAnAssert)
{
	EXPECT_DEATH((void)ember::memory::heap(static_cast<MemoryTag>(0xFFFF)), "assert");
}
#endif
