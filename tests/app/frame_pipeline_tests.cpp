#include <ember/app/frame.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <new>

using namespace ember;

/*
 * Runtime::run_frame's ordering, run headless with the real arena and the real job system: a
 * frame updates, fanning out into jobs that build what it will draw, then renders from that, and
 * the frame's memory is begun and freed exactly where the loop does it.
 */
namespace
{
	constexpr u32 WORKERS = 4;
	constexpr u32 PARTS	  = 8;	// jobs an update fans out into
	constexpr u32 WORDS	  = 61; // what each builds for render
	constexpr u64 FRAMES  = 96;

	/// What update() leaves for render(), in the frame's memory.
	struct Packet
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
			scratch.init(heap, "frame");
			before = heap.blocks_in_use();
		}

		void TearDown() override
		{
			scratch.shutdown();
			jobs::shutdown();
			EXPECT_EQ(heap.blocks_in_use(), before) << "the pipeline leaked blocks";
		}

		[[nodiscard]] u32 owned(u64 index) const noexcept
		{
			return heap.blocks_owned(heap_tag(MemoryLifetime::Frame, index));
		}

		TaggedHeap& heap = memory::tagged_heap();
		Arena scratch;
		FrameRing ring{scratch};
		Input input;
		u32 before = 0;
	};
}

TEST_F(FramePipeline, AFramesMemoryLivesUntilItsSubmit)
{
	u32 mismatches = 0;

	for (u64 index = 1; index <= FRAMES; ++index)
	{
		FrameParams& frame = ring.begin(index, 0.016f, input);
		const HeapTag tag  = heap_tag(MemoryLifetime::Frame, index);

		scratch.begin(tag);

		// update(): scribble, then build the packet in jobs, each writing its own part from
		// whichever worker runs it.
		Packet* packet = nullptr;
		{
			auto* junk = static_cast<u8*>(frame.scratch.allocate_fast(3000));
			std::memset(junk, 0xAB, 3000);

			packet		  = new (frame.scratch.allocate_fast(sizeof(Packet), alignof(Packet))) Packet{};
			packet->frame = index;

			std::atomic<u32> checksum{0};

			auto build = [&](jobs::JobRange range) noexcept
			{
				for (u32 part = range.begin; part < range.end; ++part)
				{
					auto* words = static_cast<u32*>(frame.scratch.allocate_fast(WORDS * sizeof(u32), alignof(u32)));
					u32 sum		= 0;

					for (u32 i = 0; i < WORDS; ++i)
					{
						words[i] = pattern(index, part, i);
						sum += words[i];
					}

					packet->parts[part] = words;
					checksum.fetch_add(sum, std::memory_order_relaxed);
				}
			};

			jobs::parallel_for({.count = PARTS, .grain = 1, .name = "build part"}, build);
			packet->checksum = checksum.load();
		}

		// update() has returned, and what it built is still there for render().
		EXPECT_GT(owned(index), 0u);
		EXPECT_EQ(heap.tag_of(packet), tag);

		// render(): from the packet, building beside it.
		{
			mismatches += packet->frame != index;

			u32 checksum = 0;
			for (u32 part = 0; part < PARTS; ++part)
			{
				auto* copy = static_cast<u32*>(frame.scratch.allocate_fast(WORDS * sizeof(u32), alignof(u32)));
				std::memcpy(copy, packet->parts[part], WORDS * sizeof(u32));

				for (u32 i = 0; i < WORDS; ++i)
				{
					mismatches += copy[i] != pattern(index, part, i);
					checksum += copy[i];
				}

				mismatches += heap.tag_of(packet->parts[part]) != tag;
			}

			mismatches += checksum != packet->checksum;
		}

		// The submit: the frame's memory dies, all of it at once.
		heap.free(scratch.end());

		EXPECT_EQ(owned(index), 0u);
		EXPECT_EQ(heap.blocks_in_use(), before);
	}

	EXPECT_EQ(mismatches, 0u) << "a render read what was not its frame's";
}
