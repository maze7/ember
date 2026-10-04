#include <ember/app/frame.h>
#include <ember/memory/memory.h>

#include <gtest/gtest.h>

using namespace ember;

/*
 * The ring behind App::frame(): one entry per frame, reused every CAPACITY frames, and the
 * runtime's contract that a frame is data nothing rewrites once it has begun.
 */
namespace
{
	class FrameRingTest : public ::testing::Test
	{
	protected:
		Arena scratch;
		InputState input;

		FrameRing ring{scratch};

		/// Begins frames [from, to], each stamped with dt = its own index so lookups can be checked.
		void begin_through(u64 from, u64 to)
		{
			for (u64 index = from; index <= to; ++index)
				ring.begin(index, static_cast<f32>(index), input);
		}
	};
}

TEST_F(FrameRingTest, StartsEmpty)
{
	EXPECT_EQ(ring.current_index(), 0u);
	EXPECT_EQ(ring.find(0), nullptr);
	EXPECT_EQ(ring.find(1), nullptr);
}

TEST_F(FrameRingTest, BeginsFramesInOrderAndKeepsTheLastCapacityOfThem)
{
	constexpr u64 LAST = FrameRing::CAPACITY * 2 + 5;

	begin_through(1, LAST);
	EXPECT_EQ(ring.current_index(), LAST);

	// The current frame and the CAPACITY - 1 before it are found, with their own data.
	for (u64 index = LAST - FrameRing::CAPACITY + 1; index <= LAST; ++index)
	{
		const FrameParams* frame = ring.find(index);
		ASSERT_NE(frame, nullptr) << "frame " << index;
		EXPECT_EQ(frame->frame_index, index);
		EXPECT_EQ(frame->dt, static_cast<f32>(index));
	}

	// The one before that was evicted by the current frame; nothing after the current exists.
	EXPECT_EQ(ring.find(LAST - FrameRing::CAPACITY), nullptr);
	EXPECT_EQ(ring.find(LAST + 1), nullptr);
	EXPECT_EQ(ring.find(0), nullptr);
}

TEST_F(FrameRingTest, TwoFramesCapacityApartShareAnEntry)
{
	begin_through(1, 3);
	const FrameParams* third = ring.find(3);

	begin_through(4, 3 + FrameRing::CAPACITY);
	EXPECT_EQ(ring.find(3 + FrameRing::CAPACITY), third);
	EXPECT_EQ(ring.find(3), nullptr);
}

TEST_F(FrameRingTest, BeginResetsWhatTheLastTenantLeft)
{
	FrameParams& first = ring.begin(1, 0.016f, input);

	first.frame_slot		= 2;
	first.backbuffer		= TextureHandle{.index = 7, .generation = 3};
	first.backbuffer_extent = {1280, 720};
	first.update_begin_ns	= 100;
	first.update_end_ns		= 200;
	first.render_begin_ns	= 300;
	first.render_end_ns		= 400;

	begin_through(2, FrameRing::CAPACITY);
	const FrameParams& again = ring.begin(FrameRing::CAPACITY + 1, 0.033f, input);

	EXPECT_EQ(&again, &first);
	EXPECT_EQ(again.frame_index, FrameRing::CAPACITY + 1);
	EXPECT_EQ(again.frame_slot, 0u);
	EXPECT_TRUE(again.backbuffer.is_null());
	EXPECT_EQ(again.backbuffer_extent.width, 0u);
	EXPECT_EQ(again.update_begin_ns, 0u);
	EXPECT_EQ(again.update_end_ns, 0u);
	EXPECT_EQ(again.render_begin_ns, 0u);
	EXPECT_EQ(again.render_end_ns, 0u);
	EXPECT_EQ(again.dt, 0.033f);
}

TEST_F(FrameRingTest, EveryFrameNamesTheSameArena)
{
	begin_through(1, FrameRing::CAPACITY + 3);

	for (u64 index = 4; index <= FrameRing::CAPACITY + 3; ++index)
	{
		const FrameParams* frame = ring.find(index);
		ASSERT_NE(frame, nullptr);
		EXPECT_EQ(&frame->scratch, &scratch);
	}
}

TEST_F(FrameRingTest, InputIsCopiedNotReferenced)
{
	const FrameParams& frame = ring.begin(1, 0.016f, input);
	EXPECT_NE(&frame.input, &input);
}

TEST_F(FrameRingTest, StageTimesComeFromTheStamps)
{
	FrameParams& frame = ring.begin(1, 0.016f, input);

	EXPECT_EQ(frame.update_ms(), 0.0f); // not run
	frame.update_begin_ns = 1'000'000;
	EXPECT_EQ(frame.update_ms(), 0.0f); // still running
	frame.update_end_ns = 3'500'000;
	EXPECT_FLOAT_EQ(frame.update_ms(), 2.5f);

	frame.render_begin_ns = 4'000'000;
	frame.render_end_ns	  = 4'250'000;
	EXPECT_FLOAT_EQ(frame.render_ms(), 0.25f);
}

TEST_F(FrameRingTest, ClearForgetsEveryFrame)
{
	begin_through(1, 5);
	ring.clear();

	EXPECT_EQ(ring.current_index(), 0u);
	for (u64 index = 0; index <= 6; ++index)
		EXPECT_EQ(ring.find(index), nullptr) << "frame " << index;

	// A fresh run starts at 1 again.
	EXPECT_EQ(ring.begin(1, 0.016f, input).frame_index, 1u);
}

#if !defined(NDEBUG)
TEST(FrameRingDeathTest, FramesMustBeginInOrder)
{
	Arena scratch;
	InputState input;
	FrameRing ring{scratch};

	EXPECT_DEATH((void)ring.begin(2, 0.0f, input), "assert");

	(void)ring.begin(1, 0.0f, input);
	EXPECT_DEATH((void)ring.begin(1, 0.0f, input), "assert");
	EXPECT_DEATH((void)ring.begin(3, 0.0f, input), "assert");
}
#endif

TEST_F(FrameRingTest, AFramesScratchIsMemoryUnderItsTag)
{
	TaggedHeap& heap = memory::tagged_heap();
	scratch.init(heap, "frame");
	scratch.begin(heap_tag(MemoryLifetime::Frame, 1));

	FrameParams& frame = ring.begin(1, 0.016f, input);

	auto* words = static_cast<u32*>(frame.scratch.allocate_fast(16 * sizeof(u32), alignof(u32)));
	EXPECT_EQ(heap.tag_of(words), heap_tag(MemoryLifetime::Frame, 1)) << "what a stage builds lives under the frame's tag";

	// The tag is freed by the frame loop, never by the ring.
	EXPECT_EQ(heap.free(scratch.end()), 1u);
	scratch.shutdown();
}
