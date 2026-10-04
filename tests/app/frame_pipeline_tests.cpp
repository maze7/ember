#include <ember/app/frame.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>

using namespace ember;

/*
 * Runtime::run_frame's ordering, run headless with the real arenas and the real job system: a
 * frame updates, fanning out into jobs that write its packet, then renders from that packet
 * alone, and every lifetime is begun and freed exactly where the loop does it.
 */
namespace
{
	constexpr u32 WORKERS = 4;
	constexpr u32 PARTS	  = 8;	// jobs an update fans out into
	constexpr u32 WORDS	  = 61; // what each writes into the packet
	constexpr u64 FRAMES  = 96;

	struct Payload
	{
		u64 frame		  = 0;
		u32* parts[PARTS] = {};
		u32 checksum	  = 0;
	};

	[[nodiscard]] u32 pattern(u64 frame, u32 part, u32 i) noexcept
	{
		return static_cast<u32>(frame * 7919 + part * 613 + i * 31);
	}

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

TEST_F(FramePipeline, AFramesLifetimesDieWhereItsStagesEnd)
{
	u32 mismatches = 0;

	for (u64 index = 1; index <= FRAMES; ++index)
	{
		FrameParams& frame = ring.begin(index, 0.016f, input);

		sim_scratch.begin(heap_tag(MemoryLifetime::SimScratch, index));
		sim_to_render.begin(heap_tag(MemoryLifetime::SimToRender, index));

		// update(): scribble in scratch, then build the packet in jobs, each writing its own part
		// from whichever worker runs it.
		{
			auto* junk = static_cast<u8*>(frame.sim_scratch.allocate_fast(3000));
			std::memset(junk, 0xAB, 3000);

			Payload& out = frame.publish<Payload>();
			out.frame	 = index;

			std::atomic<u32> checksum{0};

			auto build = [&](jobs::JobRange range) noexcept
			{
				for (u32 part = range.begin; part < range.end; ++part)
				{
					auto* words = static_cast<u32*>(frame.sim_to_render.allocate_fast(WORDS * sizeof(u32), alignof(u32)));
					u32 sum		= 0;

					for (u32 i = 0; i < WORDS; ++i)
					{
						words[i] = pattern(index, part, i);
						sum += words[i];
					}

					out.parts[part] = words;
					checksum.fetch_add(sum, std::memory_order_relaxed);
				}
			};

			jobs::parallel_for({.count = PARTS, .grain = 1, .name = "build part"}, build);
			out.checksum = checksum.load();

			EXPECT_TRUE(frame.sim_to_render.owns(&out));
			EXPECT_FALSE(frame.sim_scratch.owns(&out));
		}

		// update() has returned: its scratch dies, its packet closes but stays alive.
		heap.free(sim_scratch.end());
		const HeapTag packet = sim_to_render.end();

		EXPECT_EQ(owned(MemoryLifetime::SimScratch, index), 0u);
		EXPECT_GT(owned(MemoryLifetime::SimToRender, index), 0u) << "the packet outlives its update";

		// render(): from the payload alone.
		{
			render_scratch.begin(heap_tag(MemoryLifetime::RenderScratch, index));

			const Payload& in = *frame.payload<Payload>();
			mismatches += in.frame != index;

			u32 checksum = 0;
			for (u32 part = 0; part < PARTS; ++part)
			{
				auto* copy = static_cast<u32*>(frame.render_scratch.allocate_fast(WORDS * sizeof(u32), alignof(u32)));
				std::memcpy(copy, in.parts[part], WORDS * sizeof(u32));

				for (u32 i = 0; i < WORDS; ++i)
				{
					mismatches += copy[i] != pattern(index, part, i);
					checksum += copy[i];
				}

				mismatches += heap.tag_of(in.parts[part]) != packet;
			}

			mismatches += checksum != in.checksum;
			frame.gpu = {.value = index};
		}

		// The submit: the render scratch and the packet die together.
		heap.free(render_scratch.end());
		heap.free(packet);

		EXPECT_EQ(owned(MemoryLifetime::RenderScratch, index), 0u);
		EXPECT_EQ(owned(MemoryLifetime::SimToRender, index), 0u);
	}

	EXPECT_EQ(mismatches, 0u) << "a render read a payload that was not its frame's";
	EXPECT_EQ(heap.blocks_in_use(), before);
}
