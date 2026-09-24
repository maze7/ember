#include <ember/app/frame.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>

using namespace ember;

/*
 * Runtime::frame_loop's ordering, run headless with the real arenas and the real job system:
 * frame N's update runs as a job while this thread "renders" frame N - 1 from its payload, and
 * every lifetime is begun and freed exactly where the loop does it.
 */
namespace
{
	constexpr u32 WORKERS = 4;
	constexpr u32 WORDS	  = 61;

	struct Payload
	{
		u64 frame		 = 0;
		u32 words[WORDS] = {};
		u32 checksum	 = 0;
	};

	[[nodiscard]] u32 pattern(u64 frame, u32 i) noexcept { return static_cast<u32>(frame * 7919 + i * 31); }

	class FramePipeline : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			jobs::initialize({.worker_count = WORKERS});
			sim_scratch.init(heap, "sim_scratch");
			sim_to_render.init(heap, "sim_to_render");
			render_scratch.init(heap, "render_scratch");
			before = heap.blocks_in_use();
		}

		void TearDown() override
		{
			render_scratch.shutdown();
			sim_to_render.shutdown();
			sim_scratch.shutdown();
			jobs::shutdown();
			EXPECT_EQ(heap.blocks_in_use(), before) << "the pipeline leaked blocks";
		}

		[[nodiscard]] u32 owned(MemoryLifetime kind, u64 index) const noexcept
		{
			return heap.blocks_owned(heap_tag(kind, index));
		}

		TaggedHeap& heap = memory::tagged_heap();
		Arena sim_scratch;
		Arena sim_to_render;
		Arena render_scratch;
		FrameRing ring{sim_scratch, sim_to_render, render_scratch};
		InputState input;
		u32 before = 0;
	};
}

TEST_F(FramePipeline, OverlappedFramesKeepTheirLifetimesApart)
{
	constexpr u64 FRAMES = 96;

	jobs::Counter update_done;
	FrameParams* pending = nullptr; // updated, not yet rendered
	std::atomic<u32> faults{0};
	u32 mismatches = 0;

	for (u64 index = 1; index <= FRAMES; ++index)
	{
		FrameParams& frame = ring.begin(index, 0.016f, input);

		sim_scratch.begin(heap_tag(MemoryLifetime::SimScratch, index));
		sim_to_render.begin(heap_tag(MemoryLifetime::SimToRender, index));

		// The update job: scribble in scratch, publish the payload.
		auto update = [&frame, &faults]() noexcept
		{
			auto* junk = static_cast<u8*>(frame.sim_scratch.allocate_fast(3000));
			std::memset(junk, 0xAB, 3000);

			Payload& out = frame.publish<Payload>();
			out.frame	 = frame.frame_index;

			for (u32 i = 0; i < WORDS; ++i)
			{
				out.words[i] = pattern(frame.frame_index, i);
				out.checksum += out.words[i];
			}

			if (!frame.sim_to_render.owns(&out) || frame.sim_scratch.owns(&out))
				faults.fetch_add(1, std::memory_order_relaxed);
		};

		jobs::kick(jobs::make_job(update, "update"), &update_done);

		// Meanwhile, render the previous frame on this thread from its payload alone.
		if (pending != nullptr)
		{
			render_scratch.begin(heap_tag(MemoryLifetime::RenderScratch, pending->frame_index));

			const Payload& in = *pending->payload<Payload>();
			mismatches += in.frame != pending->frame_index;

			auto* copy = static_cast<u32*>(pending->render_scratch.allocate_fast(WORDS * sizeof(u32), alignof(u32)));
			std::memcpy(copy, in.words, WORDS * sizeof(u32));

			u32 checksum = 0;
			for (u32 i = 0; i < WORDS; ++i)
			{
				mismatches += copy[i] != pattern(in.frame, i);
				checksum += copy[i];
			}

			mismatches += checksum != in.checksum;
			mismatches += heap.tag_of(&in) != heap_tag(MemoryLifetime::SimToRender, in.frame);

			pending->gpu = {.value = pending->frame_index};

			// The submit: the render scratch and the packet die together.
			heap.free(render_scratch.end());
			heap.free(heap_tag(MemoryLifetime::SimToRender, pending->frame_index));

			EXPECT_EQ(owned(MemoryLifetime::RenderScratch, pending->frame_index), 0u);
			EXPECT_EQ(owned(MemoryLifetime::SimToRender, pending->frame_index), 0u);
		}

		// The join: the update's scratch dies, its packet closes but stays alive.
		jobs::wait(update_done);

		heap.free(sim_scratch.end());
		(void)sim_to_render.end();
		pending = &frame;

		EXPECT_EQ(owned(MemoryLifetime::SimScratch, index), 0u);
		EXPECT_GT(owned(MemoryLifetime::SimToRender, index), 0u) << "the packet outlives its update";
	}

	EXPECT_EQ(faults.load(), 0u) << "a payload landed in the wrong lifetime";
	EXPECT_EQ(mismatches, 0u) << "a render read a payload that was not its frame's";

	// Quit: the last frame was updated but never rendered and still owns its packet.
	heap.free(heap_tag(MemoryLifetime::SimToRender, pending->frame_index));
	EXPECT_EQ(heap.blocks_in_use(), before);
}
