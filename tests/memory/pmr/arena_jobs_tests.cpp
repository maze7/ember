#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/arena.h>
#include <ember/memory/tagged_heap.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory_resource>
#include <random>
#include <vector>

using namespace ember;
using namespace ember::jobs;

/*
 * The arena under the job system, which is where its design earns its keep:
 *
 *   1. Workers that allocate at the same time never overlap and each takes its own block.
 *   2. A container built before a wait keeps working after its fiber migrated to another
 *      worker: the same resource, a different thread's block, the same tag.
 *   3. Frame after frame of begin, produce, end, free leaves nothing behind.
 *   4. The game to render shape: frame N's packet stays intact while frame N+1's jobs allocate,
 *      and a consumer job reads it back before its tag is freed.
 *   5. Random sizes and alignments from every worker at once stay disjoint, aligned and tagged.
 *   6. The engine's own frame arena is live from initialisation and cycles through the heap.
 */

namespace
{
	struct Allocation
	{
		u8* begin	= nullptr;
		size_t size = 0;
	};

	constexpr u32 WORKERS = 4;

	/// Job system up with four workers; a private arena on the engine heap.
	class ArenaJobs : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			jobs::initialize({.worker_count = WORKERS});
			arena.init(memory::tagged_heap(), "jobs");
			before = memory::tagged_heap().blocks_in_use();
		}

		void TearDown() override
		{
			arena.shutdown();
			jobs::shutdown();
			EXPECT_EQ(memory::tagged_heap().blocks_in_use(), before) << "the test leaked blocks";
		}

		Arena arena;
		u32 before = 0;
	};

	/// Holds every job at the gate until all of them are running, which needs that many
	/// threads, so a test that says "four threads" has proven it rather than hoped for it.
	struct Gate
	{
		std::atomic<u32> arrived{0};

		void pass(u32 expected) noexcept
		{
			arrived.fetch_add(1, std::memory_order_relaxed);
			while (arrived.load(std::memory_order_relaxed) < expected)
			{
			}
		}
	};
}

TEST_F(ArenaJobs, WorkersAllocateInParallelWithoutOverlapping)
{
	constexpr u32 PER_JOB = 1024;
	constexpr u32 COUNT	  = WORKERS * PER_JOB;

	arena.begin(heap_tag(5, 1));

	std::vector<Allocation> allocations(COUNT);
	Gate gate;

	auto body = [&](JobRange range)
	{
		gate.pass(WORKERS);

		for (u32 i = range.begin; i < range.end; ++i)
		{
			const size_t size = 16 + (i % 97) * 8;
			allocations[i]	  = {static_cast<u8*>(arena.allocate_fast(size)), size};
		}
	};

	parallel_for({.count = COUNT, .grain = PER_JOB, .name = "allocate"}, body);

	std::sort(allocations.begin(), allocations.end(),
			  [](const Allocation& a, const Allocation& b) { return a.begin < b.begin; });

	for (size_t i = 0; i < allocations.size(); ++i)
	{
		ASSERT_NE(allocations[i].begin, nullptr);
		ASSERT_TRUE(arena.owns(allocations[i].begin));
		ASSERT_TRUE(arena.owns(allocations[i].begin + allocations[i].size - 1));

		if (i > 0)
		{
			ASSERT_GE(allocations[i].begin, allocations[i - 1].begin + allocations[i - 1].size)
				<< "allocation " << i << " overlaps its neighbour";
		}
	}

	// Four threads ran at once, so four slots took a block each.
	const Arena::Stats stats = arena.stats();
	EXPECT_EQ(stats.threads, WORKERS);
	EXPECT_GE(stats.refills, WORKERS);
	EXPECT_EQ(stats.blocks, memory::tagged_heap().blocks_owned(arena.tag()));

	EXPECT_EQ(memory::tagged_heap().free(arena.end()), stats.blocks);
}

TEST_F(ArenaJobs, ContainersSurviveAFiberMigration)
{
	arena.begin(heap_tag(5, 2));
	std::atomic<u32> migrated = 0;

	auto body = [&](JobRange range)
	{
		std::pmr::vector<u32> values(&arena);
		const u32 before_wait = worker_index();

		for (u32 i = 0; i < 1000; ++i)
			values.push_back(range.index);

		// A child job forces this fiber to park; it can come back on another worker.
		u32 counter = 0;
		auto child	= [&counter] { ++counter; };
		JobDef defs[]{make_job(child, "touch")};
		Batch batch(defs);
		batch.wait();

		for (u32 i = 0; i < 1000; ++i)
			values.push_back(range.index);

		ASSERT_EQ(values.size(), 2000u);
		for (u32 value : values)
			ASSERT_EQ(value, range.index);
		ASSERT_TRUE(arena.owns(values.data()));

		if (worker_index() != before_wait)
			migrated.fetch_add(1, std::memory_order_relaxed);
	};

	// Which worker resumes a parked fiber is the scheduler's choice, so one round may see no
	// migration at all. Every round checks the containers; the rounds continue until at least
	// one fiber provably came back on another worker, which a few rounds always produce.
	for (u32 round = 0; round < 50 && migrated.load() == 0; ++round)
		parallel_for({.count = 64, .grain = 1, .name = "grow"}, body);

	EXPECT_GT(migrated.load(), 0u) << "no fiber migrated in fifty rounds, the test proved nothing";
	(void)memory::tagged_heap().free(arena.end());
}

TEST_F(ArenaJobs, ManyFramesLeaveNothingBehind)
{
	TaggedHeap& heap = memory::tagged_heap();

	for (u64 frame = 1; frame <= 32; ++frame)
	{
		arena.begin(heap_tag(5, frame));

		std::atomic<size_t> produced = 0;
		auto body					 = [&](JobRange range)
		{
			for (u32 i = range.begin; i < range.end; ++i)
			{
				auto* bytes = static_cast<u8*>(arena.allocate_fast(256));
				bytes[0]	= static_cast<u8>(frame);
				produced.fetch_add(256, std::memory_order_relaxed);
			}
		};

		parallel_for({.count = 512, .grain = 8, .name = "produce"}, body);

		const HeapTag tag = arena.end();
		EXPECT_EQ(arena.stats().bytes_requested, produced.load());

		const u32 owned = heap.blocks_owned(tag);
		EXPECT_GT(owned, 0u);
		EXPECT_EQ(arena.stats().blocks, owned);
		EXPECT_EQ(heap.free(tag), owned);
		EXPECT_EQ(heap.free(tag), 0u);
		EXPECT_EQ(heap.blocks_owned(tag), 0u);
	}

	EXPECT_EQ(heap.blocks_in_use(), before);
}

/*
 * The game to render lifetime, one frame ahead: frame N's jobs write a packet into the arena,
 * the arena moves on to frame N+1 whose jobs allocate hard, and only then does a consumer job
 * read frame N's packet back. Every byte has to survive, and the tag is freed afterwards.
 */
TEST_F(ArenaJobs, APacketSurvivesTheNextFramesAllocations)
{
	TaggedHeap& heap = memory::tagged_heap();

	struct Packet
	{
		u32* values = nullptr;
		u32 count	= 0;
		u64 frame	= 0;
	};

	constexpr u32 VALUES   = 8192;
	HeapTag previous_tag   = NO_TAG;
	const Packet* previous = nullptr;

	for (u64 frame = 1; frame <= 8; ++frame)
	{
		arena.begin(heap_tag(6, frame));

		// Produce this frame's packet from several jobs.
		auto* packet   = ::new (arena.allocate_fast(sizeof(Packet), alignof(Packet))) Packet{};
		packet->values = static_cast<u32*>(arena.allocate_fast(VALUES * sizeof(u32), alignof(u32)));
		packet->count  = VALUES;
		packet->frame  = frame;

		auto produce = [&](JobRange range)
		{
			for (u32 i = range.begin; i < range.end; ++i)
				packet->values[i] = static_cast<u32>(frame) * 1000003u ^ i;
		};
		parallel_for({.count = VALUES, .grain = 256, .name = "produce"}, produce);

		const HeapTag tag = arena.end();

		// Consume the previous frame's packet now, while this frame's memory has been written
		// all over the heap, then free it.
		if (previous != nullptr)
		{
			std::atomic<u32> bad{0};
			auto consume = [&](JobRange range)
			{
				for (u32 i = range.begin; i < range.end; ++i)
					if (previous->values[i] != (static_cast<u32>(previous->frame) * 1000003u ^ i))
						bad.fetch_add(1, std::memory_order_relaxed);
			};
			parallel_for({.count = previous->count, .grain = 256, .name = "consume"}, consume);

			EXPECT_EQ(bad.load(), 0u) << "frame " << previous->frame << " was overwritten by frame " << frame;
			EXPECT_EQ(heap.tag_of(previous), previous_tag);
			EXPECT_GT(heap.free(previous_tag), 0u);
		}

		previous	 = packet;
		previous_tag = tag;
	}

	EXPECT_GT(heap.free(previous_tag), 0u);
	EXPECT_EQ(heap.blocks_in_use(), before);
}

TEST_F(ArenaJobs, RandomSizesAndAlignmentsFromEveryWorkerStayDisjointAndAligned)
{
	constexpr u32 JOBS	  = 32;
	constexpr u32 PER_JOB = 300;

	arena.begin(heap_tag(5, 3));

	std::vector<std::vector<Allocation>> per_job(JOBS);
	std::atomic<u32> misaligned{0};
	std::atomic<u32> foreign{0};

	auto body = [&](JobRange range)
	{
		for (u32 job = range.begin; job < range.end; ++job)
		{
			std::mt19937 rng(job + 1);
			std::vector<Allocation>& mine = per_job[job];
			mine.reserve(PER_JOB);

			for (u32 i = 0; i < PER_JOB; ++i)
			{
				const size_t size	   = rng() % 3000;
				const size_t alignment = size_t{1} << (rng() % 8);
				auto* ptr			   = static_cast<u8*>(arena.allocate_fast(size, alignment));
				const size_t extent	   = size == 0 ? 1 : size;

				if (!is_aligned(ptr, alignment))
					misaligned.fetch_add(1, std::memory_order_relaxed);
				if (!arena.owns(ptr) || !arena.owns(ptr + extent - 1))
					foreign.fetch_add(1, std::memory_order_relaxed);

				std::memset(ptr, static_cast<int>(job), extent); // stamp it
				mine.push_back({ptr, extent});
			}
		}
	};

	parallel_for({.count = JOBS, .grain = 1, .name = "random"}, body);

	EXPECT_EQ(misaligned.load(), 0u);
	EXPECT_EQ(foreign.load(), 0u);

	// Every stamp intact: nobody wrote into anybody else's allocation.
	for (u32 job = 0; job < JOBS; ++job)
		for (const Allocation& a : per_job[job])
			for (size_t i = 0; i < a.size; i += 97)
				ASSERT_EQ(a.begin[i], static_cast<u8>(job)) << "job " << job << " lost bytes to another job";

	std::vector<Allocation> all;
	for (const auto& mine : per_job)
		all.insert(all.end(), mine.begin(), mine.end());
	std::sort(all.begin(), all.end(), [](const Allocation& a, const Allocation& b) { return a.begin < b.begin; });
	for (size_t i = 1; i < all.size(); ++i)
		ASSERT_GE(all[i].begin, all[i - 1].begin + all[i - 1].size) << "allocation " << i << " overlaps";

	(void)memory::tagged_heap().free(arena.end());
}

/// One turn of Runtime::frame_loop, several times over: the game stage fills its scratch and
/// publishes a packet on every worker, the render stage reads the packet while filling its own
/// scratch, and the three lifetimes retire together at the top of the next frame. Nothing leaks,
/// nothing mixes, and the packet is intact when render reads it.
TEST_F(ArenaJobs, ThreeLifetimesCycleLikeTheRuntimeLoop)
{
	constexpr u32 FRAMES	= 8;
	constexpr u32 PER_SLICE = 64;

	TaggedHeap& heap = memory::tagged_heap();

	// The fixture's arena plays game scratch; the other two lifetimes are locals on the same heap.
	Arena& game_scratch = arena;
	Arena game_to_render;
	Arena render_scratch;

	game_to_render.init(heap, "game_to_render");
	render_scratch.init(heap, "render_scratch");

	for (u32 frame = 0; frame < FRAMES; ++frame)
	{
		const u64 seq = frame + 1;

		game_scratch.begin(heap_tag(MemoryLifetime::SimScratch, seq));
		game_to_render.begin(heap_tag(MemoryLifetime::SimToRender, seq));
		render_scratch.begin(heap_tag(MemoryLifetime::RenderScratch, seq));

		EXPECT_EQ(kind(game_to_render.tag()), static_cast<u8>(MemoryLifetime::SimToRender));
		EXPECT_EQ(sequence(game_to_render.tag()), seq);

		// Game stage: every job scribbles in scratch and publishes one slice of the packet.
		const u32* packet[WORKERS] = {};

		auto game = [&](JobRange range)
		{
			for (u32 i = range.begin; i < range.end; ++i)
			{
				void* junk = game_scratch.allocate_fast(1024);
				std::memset(junk, 0xAB, 1024);

				auto* slice = static_cast<u32*>(game_to_render.allocate_fast(PER_SLICE * sizeof(u32), alignof(u32)));
				for (u32 k = 0; k < PER_SLICE; ++k)
					slice[k] = frame * 1000 + i * PER_SLICE + k;

				packet[i] = slice;
			}
		};

		parallel_for({.count = WORKERS, .grain = 1, .name = "game"}, game);

		// Render stage: reads the packet on whichever worker, allocating only from its own scratch.
		std::atomic<u32> faults{0};

		auto render = [&](JobRange range)
		{
			for (u32 i = range.begin; i < range.end; ++i)
			{
				const u32* slice = packet[i];
				auto* copy = static_cast<u32*>(render_scratch.allocate_fast(PER_SLICE * sizeof(u32), alignof(u32)));
				std::memcpy(copy, slice, PER_SLICE * sizeof(u32));

				for (u32 k = 0; k < PER_SLICE; ++k)
					if (copy[k] != frame * 1000 + i * PER_SLICE + k)
						faults.fetch_add(1, std::memory_order_relaxed);

				// Ownership is exact: the slice is game to render memory and nothing else's.
				if (!game_to_render.owns(slice) || game_scratch.owns(slice) || render_scratch.owns(slice) ||
					!render_scratch.owns(copy))
					faults.fetch_add(1, std::memory_order_relaxed);
			}
		};

		parallel_for({.count = WORKERS, .grain = 1, .name = "render"}, render);

		EXPECT_EQ(faults.load(), 0u) << "frame " << frame;
		EXPECT_GT(heap.blocks_in_use(), before);

		// Top of the next frame: all three retire at once and the heap is back where it started.
		EXPECT_GT(heap.free(game_scratch.end()), 0u);
		EXPECT_GT(heap.free(game_to_render.end()), 0u);
		EXPECT_GT(heap.free(render_scratch.end()), 0u);
		EXPECT_EQ(heap.blocks_in_use(), before) << "frame " << frame << " leaked";
	}

	render_scratch.shutdown();
	game_to_render.shutdown();
}
