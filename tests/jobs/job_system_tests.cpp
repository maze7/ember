#include <ember/jobs/job_system.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#if defined(EMBER_PLATFORM_LINUX)
	#include <pthread.h>
	#include <sched.h>
#endif

using namespace ember;
using namespace ember::jobs;

namespace
{
	struct State
	{
		std::atomic<u32> ran{0};
		std::thread::id	 main_thread;
		u32				 worker_in_main		 = NO_WORKER;
		bool			 complete_after_kick = true;
		u32				 ran_after_two		 = 0;
		bool			 large_ok			 = false;
		bool			 small_ok			 = false;
		bool			 stale_reads_complete = false;
		bool			 slot_reused		 = false;
		bool			 moved_from_is_null	 = false;
		std::atomic<u64> worker_mask{0};
		std::atomic<u32> started{0};
		std::atomic<bool> timed_out{false};
		u32				 parallel_count = 0;
		std::atomic<u32> named_ok{0};
		std::atomic<u32> pinned_ok{0};
		std::atomic<u32> on_workers{0};
	};

	void count_job(void* data)
	{
		static_cast<State*>(data)->ran.fetch_add(1, std::memory_order_relaxed);
	}

	void fill(JobDef* decls, u32 count, void* data)
	{
		for (u32 i = 0; i < count; ++i)
			decls[i] = {.fn = count_job, .data = data};
	}

	void main_kick_and_wait(void* data)
	{
		auto& state			  = *static_cast<State*>(data);
		state.main_thread	  = std::this_thread::get_id();
		state.worker_in_main = worker_index();

		JobDef decls[16];
		fill(decls, 16, &state);

		JobHandle batch			  = kick(decls);
		state.complete_after_kick = is_complete(batch);
		wait(batch);
		release(batch);
	}

	void parent_job(void* data)
	{
		JobDef children[4];
		fill(children, 4, data);

		JobBatch batch(children);
		batch.wait();
		count_job(data);
	}

	void main_nested(void* data)
	{
		JobDef parents[4];
		for (JobDef& parent : parents)
			parent = {.fn = parent_job, .data = data};

		JobBatch batch(parents);
		batch.wait();
	}

	void main_partial_wait(void* data)
	{
		auto& state = *static_cast<State*>(data);

		JobDef decls[4];
		fill(decls, 4, &state);

		JobBatch batch(decls);
		batch.wait(2);
		state.ran_after_two = state.ran.load();
		batch.wait();
	}

	void large_job(void* data)
	{
		volatile u8 buffer[300 * 1024];
		buffer[0]					= 1;
		buffer[sizeof(buffer) - 1]	= 2;
		static_cast<State*>(data)->large_ok = buffer[0] == 1 && buffer[sizeof(buffer) - 1] == 2;
	}

	void small_job(void* data)
	{
		volatile u8 buffer[40 * 1024];
		buffer[0]					= 1;
		buffer[sizeof(buffer) - 1]	= 2;
		static_cast<State*>(data)->small_ok = buffer[0] == 1 && buffer[sizeof(buffer) - 1] == 2;
	}

	void main_stack_classes(void* data)
	{
		JobBatch batch;
		batch.kick({.fn = large_job, .data = data, .stack = JobStack::Large});
		batch.kick({.fn = small_job, .data = data});
		batch.wait();
	}

	void main_recycle(void* data)
	{
		for (u32 round = 0; round < 200; ++round)
		{
			JobBatch batch({.fn = parent_job, .data = data});
			batch.wait();
		}
	}

	void main_stale_handles(void* data)
	{
		auto& state = *static_cast<State*>(data);

		JobDef decls[2];
		fill(decls, 2, &state);

		JobHandle first = kick(decls);
		wait(first);
		release(first);
		state.stale_reads_complete = is_complete(first);
		wait(first); // returns at once

		// The pool has two slots: the next batch takes the other one, the one after reuses
		// the first slot with a new generation, so the old handle still reads as complete.
		JobHandle second = kick(decls);
		JobHandle third	 = kick(decls);
		state.slot_reused = third.index == first.index && third != first && is_complete(first) && !is_complete(third);
		wait(second);
		wait(third);
		release(second);
		release(third);
	}

	void main_detached(void* data)
	{
		auto& state = *static_cast<State*>(data);

		JobDef decls[3];
		fill(decls, 3, &state);

		// A detached batch keeps its slot until its last job finishes. While main waits on
		// the second batch both run, and the slot comes back for the third.
		JobBatch detached(decls);
		const JobHandle detached_handle = detached.handle();
		detached.detach();

		{
			JobBatch second(decls);
			second.wait();
		}

		{
			JobBatch third(decls);
			state.slot_reused = third.handle().index == detached_handle.index && is_complete(detached_handle);
			third.wait();
		}

		// A detached kick runs the next time anything waits.
		kick_detached({.fn = count_job, .data = data});
		JobBatch flush({.fn = count_job, .data = data});
		flush.wait();
	}

	void main_moves(void* data)
	{
		auto& state = *static_cast<State*>(data);

		JobDef decls[2];
		fill(decls, 2, &state);

		JobBatch original(decls);
		JobBatch moved			 = std::move(original);
		state.moved_from_is_null = original.handle().is_null();

		std::vector<JobBatch> batches;
		for (u32 i = 0; i < 4; ++i)
			batches.emplace_back(decls);

		batches.push_back(std::move(moved));

		for (JobBatch& batch : batches)
			batch.wait();
	}

	// Every job waits for all of them to have started, so the batch only completes if they
	// run at the same time. The deadline turns a missing worker into a failure, not a hang.
	void barrier_job(void* data)
	{
		auto& state = *static_cast<State*>(data);
		state.started.fetch_add(1);

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

		while (state.started.load() < state.parallel_count)
		{
			if (std::chrono::steady_clock::now() > deadline)
			{
				state.timed_out.store(true);
				return;
			}

			std::this_thread::yield();
		}
	}

	void main_parallel(void* data)
	{
		auto& state			 = *static_cast<State*>(data);
		state.parallel_count = 4;

		JobDef decls[4];
		for (JobDef& decl : decls)
			decl = {.fn = barrier_job, .data = data};

		JobBatch batch(decls);
		batch.wait();
	}

	void worker_mask_job(void* data)
	{
		auto& state = *static_cast<State*>(data);
		state.worker_mask.fetch_or(u64{1} << worker_index(), std::memory_order_relaxed);

		const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
		while (std::chrono::steady_clock::now() < until)
		{
		}
	}

	void main_spread(void* data)
	{
		JobDef decls[64];
		for (JobDef& decl : decls)
			decl = {.fn = worker_mask_job, .data = data};

		JobBatch batch(decls);
		batch.wait();
	}

	// Every job burns a little time first, so the batch lasts long enough for the sleeping
	// workers to wake and take their share.
	void inspect_thread_job(void* data)
	{
		auto& state = *static_cast<State*>(data);

		const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
		while (std::chrono::steady_clock::now() < until)
		{
		}

		const u32 worker = worker_index();

		if (worker == 0)
			return; // main's thread keeps its own name and affinity

		state.on_workers.fetch_add(1);

#if defined(EMBER_PLATFORM_LINUX)
		char name[32] = {};
		pthread_getname_np(pthread_self(), name, sizeof(name));
		if (std::strncmp(name, "ember.jobs.", 11) == 0)
			state.named_ok.fetch_add(1);

		if (static_cast<u32>(sched_getcpu()) == worker % std::thread::hardware_concurrency())
			state.pinned_ok.fetch_add(1);
#else
		state.named_ok.fetch_add(1);
		state.pinned_ok.fetch_add(1);
#endif
	}

	void main_inspect_threads(void* data)
	{
		JobDef decls[64];
		for (JobDef& decl : decls)
			decl = {.fn = inspect_thread_job, .data = data};

		JobBatch batch(decls);
		batch.wait();
	}

	void main_stress(void* data)
	{
		for (u32 round = 0; round < 50; ++round)
		{
			JobDef parents[16];
			for (JobDef& parent : parents)
				parent = {.fn = parent_job, .data = data};

			JobBatch batch(parents);
			batch.wait();
		}
	}
}

TEST(JobSystem, KickAndWaitRunsEveryJob)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2});
	jobs.run(main_kick_and_wait, &state);

	EXPECT_EQ(state.ran.load(), 16u);
	EXPECT_EQ(state.main_thread, std::this_thread::get_id());
	EXPECT_EQ(state.worker_in_main, 0u);
	EXPECT_EQ(worker_index(), NO_WORKER);
	EXPECT_EQ(worker_count(), 4u);
}

TEST(JobSystem, NestedKicksAndWaits)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2});
	jobs.run(main_nested, &state);

	EXPECT_EQ(state.ran.load(), 20u);
}

TEST(JobSystem, WaitsForAnIntermediateValueSingleWorker)
{
	State	  state;
	JobSystem jobs({.worker_count = 1, .small_fibers = 8, .large_fibers = 2});
	jobs.run(main_partial_wait, &state);

	EXPECT_EQ(state.ran_after_two, 2u);
	EXPECT_EQ(state.ran.load(), 4u);
}

TEST(JobSystem, WaitsForAnIntermediateValueWithWorkers)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2});
	jobs.run(main_partial_wait, &state);

	EXPECT_GE(state.ran_after_two, 2u);
	EXPECT_EQ(state.ran.load(), 4u);
}

TEST(JobSystem, LargeJobsRunOnLargeStacks)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2});
	jobs.run(main_stack_classes, &state);

	EXPECT_TRUE(state.large_ok);
	EXPECT_TRUE(state.small_ok);
}

TEST(JobSystem, FibersAreRecycled)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 8, .large_fibers = 2});
	jobs.run(main_recycle, &state);

	EXPECT_EQ(state.ran.load(), 200u * 5u);
}

TEST(JobSystem, RunsTwice)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2});
	jobs.run(main_nested, &state);
	jobs.run(main_nested, &state);

	EXPECT_EQ(state.ran.load(), 40u);
}

TEST(JobSystem, StaleHandlesReadComplete)
{
	State	  state;
	JobSystem jobs({.worker_count = 1, .small_fibers = 4, .large_fibers = 2, .counter_capacity = 2});
	jobs.run(main_stale_handles, &state);

	EXPECT_TRUE(state.stale_reads_complete);
	EXPECT_TRUE(state.slot_reused);
	EXPECT_EQ(state.ran.load(), 6u);
}

TEST(JobSystem, DetachedBatchesFreeThemselves)
{
	State	  state;
	JobSystem jobs({.worker_count = 1, .small_fibers = 4, .large_fibers = 2, .counter_capacity = 2});
	jobs.run(main_detached, &state);

	EXPECT_TRUE(state.slot_reused);
	EXPECT_EQ(state.ran.load(), 11u);
}

TEST(JobSystem, BatchesMove)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2});
	jobs.run(main_moves, &state);

	EXPECT_TRUE(state.moved_from_is_null);
	EXPECT_EQ(state.ran.load(), 10u);
}

TEST(JobSystem, JobsRunInParallel)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2});
	jobs.run(main_parallel, &state);

	EXPECT_FALSE(state.timed_out.load());
	EXPECT_EQ(state.started.load(), 4u);
}

TEST(JobSystem, JobsSpreadAcrossWorkers)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2});
	jobs.run(main_spread, &state);

	EXPECT_GE(std::popcount(state.worker_mask.load()), 2);
	EXPECT_EQ(state.worker_mask.load() & ~u64{0xF}, 0u);
}

TEST(JobSystem, KicksFromAnotherThreadWithoutRun)
{
	State	  state;
	JobSystem jobs({.worker_count = 3, .small_fibers = 16, .large_fibers = 2});

	std::thread producer(
		[&]
		{
			for (u32 round = 0; round < 8; ++round)
			{
				JobDef decls[8];
				fill(decls, 8, &state);

				const JobHandle batch = kick(decls);

				while (!is_complete(batch))
					std::this_thread::yield();

				release(batch);
			}
		});

	producer.join();
	EXPECT_EQ(state.ran.load(), 64u);
}

TEST(JobSystem, WaitsFromAThreadOutsideTheSystem)
{
	State	  state;
	JobSystem jobs({.worker_count = 3, .small_fibers = 16, .large_fibers = 2});

	std::thread outsider(
		[&]
		{
			JobDef decls[8];
			fill(decls, 8, &state);

			JobBatch batch(decls);
			batch.wait(); // blocks this thread, there is no fiber to park

			const JobHandle handle = kick(decls);
			wait(handle);
			release(handle);
		});

	outsider.join();
	EXPECT_EQ(state.ran.load(), 16u);
}

TEST(JobSystem, WorkersAreNamedAndPinned)
{
	State	  state;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2, .pin_workers = true});
	jobs.run(main_inspect_threads, &state);

	EXPECT_GT(state.on_workers.load(), 0u);
	EXPECT_EQ(state.named_ok.load(), state.on_workers.load());
	EXPECT_EQ(state.pinned_ok.load(), state.on_workers.load());
}

TEST(JobSystem, StressNestedBatches)
{
	State	  state;
	JobSystem jobs({.worker_count = 8, .small_fibers = 48, .large_fibers = 4});
	jobs.run(main_stress, &state);

	EXPECT_EQ(state.ran.load(), 50u * 16u * 5u);
}

namespace
{
	void nop_job(void*) {}

	struct Bench
	{
		f64 round_trip_ns = 0;
		f64 per_job_ns	  = 0;
	};

	void main_bench(void* data)
	{
		auto& bench = *static_cast<Bench*>(data);

		{
			constexpr u32 iterations = 100'000;
			const auto	  start		 = std::chrono::steady_clock::now();
			for (u32 i = 0; i < iterations; ++i)
			{
				JobBatch batch({.fn = nop_job});
				batch.wait();
			}
			bench.round_trip_ns =
				std::chrono::duration<f64, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations;
		}

		{
			constexpr u32 batches = 4'000;
			JobDef		  decls[64];
			for (JobDef& decl : decls)
				decl = {.fn = nop_job};

			const auto start = std::chrono::steady_clock::now();
			for (u32 i = 0; i < batches; ++i)
			{
				JobBatch batch(decls);
				batch.wait();
			}
			bench.per_job_ns =
				std::chrono::duration<f64, std::nano>(std::chrono::steady_clock::now() - start).count() / (batches * 64.0);
		}
	}
}

TEST(JobSystem, Bench)
{
	Bench	  bench;
	JobSystem jobs({.worker_count = 4, .small_fibers = 16, .large_fibers = 2});
	jobs.run(main_bench, &bench);
	std::printf("[          ] %.1f ns per one-job kick+wait, %.1f ns per job in 64-job batches, 4 workers\n",
				bench.round_trip_ns, bench.per_job_ns);
}
