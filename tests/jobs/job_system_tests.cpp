#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>
#include <ember/sync/spin_mutex.h>
#include <ember/sync/thread.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(EMBER_PLATFORM_LINUX)
	#include <pthread.h>
	#include <sched.h>
	#include <unistd.h>
#endif

#include <csignal>
#include <sys/wait.h>

using namespace ember;
using namespace ember::jobs;

namespace
{
	struct State
	{
		std::atomic<u32> ran{0};
		std::thread::id main_thread;
		u32 worker_in_main		 = NO_WORKER;
		bool complete_after_kick = true;
		u32 ran_after_two		 = 0;
		bool deep_ok			 = false;
		std::atomic<u64> worker_mask{0};
		std::atomic<u32> started{0};
		std::atomic<bool> timed_out{false};
		u32 parallel_count = 0;
		std::atomic<u32> named_ok{0};
		std::atomic<u32> pinned_ok{0};
		std::atomic<u32> on_workers{0};
	};

	void nop_job(void*) {}

	void count_job(void* data) { static_cast<State*>(data)->ran.fetch_add(1, std::memory_order_relaxed); }

	void fill(JobDef* decls, u32 count, void* data)
	{
		for (u32 i = 0; i < count; ++i)
			decls[i] = {.fn = count_job, .data = data};
	}

	void main_kick_and_wait(void* data)
	{
		auto& state			 = *static_cast<State*>(data);
		state.main_thread	 = std::this_thread::get_id();
		state.worker_in_main = worker_index();

		JobDef decls[16];
		fill(decls, 16, &state);

		Counter done;
		kick(decls, &done);
		state.complete_after_kick = done.is_complete();
		jobs::wait(done);
	}

	void main_wait_under_spin_lock(void*)
	{
		SpinMutex mutex;
		std::lock_guard lock(mutex);

		Batch child({.fn = nop_job});
		child.wait();
	}

	void parent_job(void* data)
	{
		JobDef children[4];
		fill(children, 4, data);

		Batch batch(children);
		batch.wait();
		count_job(data);
	}

	void main_nested(void* data)
	{
		JobDef parents[4];
		for (JobDef& parent : parents)
			parent = {.fn = parent_job, .data = data};

		Batch batch(parents);
		batch.wait();
	}

	// A frame most of the way to the default stack size.
	void deep_job(void* data)
	{
		volatile u8 buffer[200 * 1024];
		buffer[0]						   = 1;
		buffer[sizeof(buffer) - 1]		   = 2;
		static_cast<State*>(data)->deep_ok = buffer[0] == 1 && buffer[sizeof(buffer) - 1] == 2;
	}

	void main_deep(void* data)
	{
		Batch batch({.fn = deep_job, .data = data});
		batch.wait();
	}

	void main_recycle(void* data)
	{
		for (u32 round = 0; round < 200; ++round)
		{
			Batch batch({.fn = parent_job, .data = data});
			batch.wait();
		}
	}

	// Two kicks share one counter and one wait, then a fire-and-forget kick runs the next
	// time anything waits.
	void main_shared_counter(void* data)
	{
		auto& state = *static_cast<State*>(data);

		JobDef decls[4];
		fill(decls, 4, &state);

		Counter done;
		kick(decls, &done);
		kick(decls, &done);
		jobs::wait(done);
		state.ran_after_two = state.ran.load();

		kick({.fn = count_job, .data = data});
		Batch flush({.fn = count_job, .data = data});
		flush.wait();
	}

	struct Grow
	{
		Counter* counter;
		State* state;
	};

	// Kicks children onto the counter its parent waits on and returns without waiting.
	void grow_job(void* data)
	{
		auto& grow = *static_cast<Grow*>(data);

		JobDef children[4];
		fill(children, 4, grow.state);
		kick(children, grow.counter);

		count_job(grow.state);
	}

	void main_grow(void* data)
	{
		auto& state = *static_cast<State*>(data);

		Counter done;
		Grow grow{&done, &state};

		JobDef decls[4];
		for (JobDef& decl : decls)
			decl = {.fn = grow_job, .data = &grow};

		kick(decls, &done);
		jobs::wait(done);
		state.ran_after_two = state.ran.load(); // 4 parents and 16 children, or the wait returned early
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

		Batch batch(decls);
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

		Batch batch(decls);
		batch.wait();
	}

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

		Batch batch(decls);
		batch.wait();
	}

	void main_stress(void* data)
	{
		for (u32 round = 0; round < 50; ++round)
		{
			JobDef parents[16];
			for (JobDef& parent : parents)
				parent = {.fn = parent_job, .data = data};

			Batch batch(parents);
			batch.wait();
		}
	}

	struct RangeState
	{
		std::vector<u8> hits;
		u32 grain = 1;
		std::atomic<u32> visited{0};
		std::atomic<u32> jobs{0};
		std::atomic<u32> bad_index{0};
		u32 sizes[MAX_RANGE_JOBS] = {};
	};

	void main_parallel_for(void* data)
	{
		auto& state = *static_cast<RangeState*>(data);

		auto visit = [&](JobRange range)
		{
			for (u32 i = range.begin; i < range.end; ++i)
				state.hits[i] += 1;

			state.visited.fetch_add(range.count(), std::memory_order_relaxed);
			state.jobs.fetch_add(1, std::memory_order_relaxed);

			if (range.index < MAX_RANGE_JOBS)
				state.sizes[range.index] = range.count();
			else
				state.bad_index.fetch_add(1, std::memory_order_relaxed);
		};

		parallel_for({.count = static_cast<u32>(state.hits.size()), .grain = state.grain, .name = "visit"}, visit);
	}

	void main_parallel_for_nested(void* data)
	{
		auto& state = *static_cast<RangeState*>(data);

		auto outer = [&](JobRange blocks)
		{
			for (u32 block = blocks.begin; block < blocks.end; ++block)
			{
				auto inner = [&](JobRange range)
				{
					for (u32 i = range.begin; i < range.end; ++i)
						state.hits[block * 1000 + i] += 1;

					state.visited.fetch_add(range.count(), std::memory_order_relaxed);
				};

				parallel_for({.count = 1000, .grain = 100, .name = "inner"}, inner);
			}
		};

		parallel_for({.count = 8, .grain = 1, .name = "outer"}, outer);
	}

	void main_make_job(void* data)
	{
		auto& state = *static_cast<State*>(data);

		u32 first  = 0;
		u32 second = 0;
		auto a	   = [&] { first = 1; };
		auto b	   = [&] { second = 2; };

		const JobDef decls[] = {make_job(a, "a"), make_job(b, "b")};

		Batch batch(decls);
		batch.wait();

		state.ran_after_two = first + second;
	}

	[[nodiscard]] u64 os_thread_id() { return static_cast<u64>(gettid()); }

	void spin_job(void*)
	{
		const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
		while (std::chrono::steady_clock::now() < until)
		{
		}
	}

	struct MigrationState
	{
		std::atomic<u32> migrated{0};
		std::atomic<u32> mismatched{0};
	};

	void migrating_job(void* data)
	{
		auto& state = *static_cast<MigrationState*>(data);

		const u64 os_before = os_thread_id();
		const u32 id_before = current_thread_id();
		void* block			= memory::heap(MemoryTag::Engine).allocate(64, 16);

		Batch child({.fn = spin_job, .name = "spin"});
		child.wait();

		const u64 os_after = os_thread_id();
		const u32 id_after = current_thread_id();
		memory::heap(MemoryTag::Engine).deallocate(block, 64, 16);

		const bool moved = os_after != os_before;
		if (moved)
			state.migrated.fetch_add(1, std::memory_order_relaxed);
		if (moved != (id_after != id_before))
			state.mismatched.fetch_add(1, std::memory_order_relaxed);
	}

	void main_migration(void* data)
	{
		for (u32 round = 0; round < 20; ++round)
		{
			JobDef decls[32];
			for (JobDef& decl : decls)
				decl = {.fn = migrating_job, .data = data, .name = "migrating"};

			Batch batch(decls);
			batch.wait();
		}
	}

	struct StallState
	{
		std::atomic<u32> ran{0};
		JobStats stats;
	};

	void stall_child(void* data) { static_cast<StallState*>(data)->ran.fetch_add(1, std::memory_order_relaxed); }

	void stall_parent(void* data)
	{
		JobDef children[4];
		for (JobDef& child : children)
			child = {.fn = stall_child, .data = data, .name = "child"};

		Batch batch(children);
		batch.wait();

		static_cast<StallState*>(data)->ran.fetch_add(1, std::memory_order_relaxed);
	}

	void holder_job(void* data)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

		while (stats().stalls < 3 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();

		static_cast<StallState*>(data)->ran.fetch_add(1, std::memory_order_relaxed);
	}

	// Eight fibers, three taken by worker threads and one by main's wait: four parents park
	// and the next three find nothing.
	void main_stall(void* data)
	{
		auto& state = *static_cast<StallState*>(data);

		JobDef decls[8];
		decls[0] = {.fn = holder_job, .data = data, .name = "holder"};
		for (u32 i = 1; i < 8; ++i)
			decls[i] = {.fn = stall_parent, .data = data, .name = "parent"};

		Batch batch(decls);
		batch.wait();

		state.stats = stats();
	}

	void detached_parent(void* data)
	{
		JobDef children[4];
		fill(children, 4, data);

		kick(children);
		count_job(data);
	}

	struct CycleState
	{
		std::atomic<Counter*> parents{nullptr};
	};

	void cycle_child(void* data)
	{
		auto& state = *static_cast<CycleState*>(data);

		Counter* parents = nullptr;
		while ((parents = state.parents.load(std::memory_order_acquire)) == nullptr)
			std::this_thread::yield();

		jobs::wait(*parents);
	}

	// Each parent waits for a child that waits for every parent, so nothing can finish.
	void cycle_parent(void* data)
	{
		Batch child({.fn = cycle_child, .data = data, .name = "cycle child"});
		child.wait();
	}

	void main_cycle(void* data)
	{
		auto& state = *static_cast<CycleState*>(data);

		JobDef parents[8];
		for (JobDef& parent : parents)
			parent = {.fn = cycle_parent, .data = data, .name = "cycle parent"};

		Batch batch(parents);
		state.parents.store(&batch.counter(), std::memory_order_release);
		batch.wait();
	}

	bool died_fatally(int status)
	{
		return WIFSIGNALED(status) &&
			   (WTERMSIG(status) == SIGILL || WTERMSIG(status) == SIGTRAP || WTERMSIG(status) == SIGABRT);
	}

	struct SignalState
	{
		std::atomic<bool> signalled{false};
		bool woke_after_signal = false;
		u32 worker_before	   = NO_WORKER;
		u32 worker_after	   = NO_WORKER;
	};

	// An outside thread stands in for the I/O thread: it finishes on its own clock and signals.
	void main_manual_counter(void* data)
	{
		auto& state = *static_cast<SignalState*>(data);

		Counter io{1};

		std::thread thread(
			[&]
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				state.signalled.store(true, std::memory_order_release);
				jobs::signal(io);
			});

		state.worker_before = worker_index();
		jobs::wait(io); // parks main; worker 0 is free for other work until the signal
		state.woke_after_signal = state.signalled.load(std::memory_order_acquire);
		state.worker_after		= worker_index();

		thread.join();
	}

	struct FanInState
	{
		std::atomic<u32> signals{0};
		std::atomic<u32> woken{0};
		std::atomic<u32> early{0};
	};

	struct FanInJob
	{
		FanInState* state;
		Counter* counter;
	};

	void fan_in_waiter(void* data)
	{
		const auto& job = *static_cast<const FanInJob*>(data);
		jobs::wait(*job.counter);

		if (job.state->signals.load(std::memory_order_acquire) != 3)
			job.state->early.fetch_add(1, std::memory_order_relaxed);

		job.state->woken.fetch_add(1, std::memory_order_relaxed);
	}

	// Eight fibers park on one counter that three outside threads bring down together.
	void main_fan_in(void* data)
	{
		auto& state = *static_cast<FanInState*>(data);

		Counter counter{3};

		FanInJob params[8];
		JobDef decls[8];
		for (u32 i = 0; i < 8; ++i)
		{
			params[i] = {&state, &counter};
			decls[i]  = {.fn = fan_in_waiter, .data = &params[i], .name = "waiter"};
		}

		Batch waiters(decls);

		std::thread signallers[3];
		for (std::thread& thread : signallers)
		{
			thread = std::thread(
				[&]
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					state.signals.fetch_add(1, std::memory_order_acq_rel);
					jobs::signal(counter);
				});
		}

		waiters.wait();

		for (std::thread& thread : signallers)
			thread.join();
	}

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
			const auto start		 = std::chrono::steady_clock::now();
			for (u32 i = 0; i < iterations; ++i)
			{
				Batch batch({.fn = nop_job});
				batch.wait();
			}
			bench.round_trip_ns =
				std::chrono::duration<f64, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations;
		}

		{
			constexpr u32 batches = 4'000;
			JobDef decls[64];
			for (JobDef& decl : decls)
				decl = {.fn = nop_job};

			const auto start = std::chrono::steady_clock::now();
			for (u32 i = 0; i < batches; ++i)
			{
				Batch batch(decls);
				batch.wait();
			}
			bench.per_job_ns = std::chrono::duration<f64, std::nano>(std::chrono::steady_clock::now() - start).count() /
							   (batches * 64.0);
		}
	}
}

TEST(JobSystem, KickAndWaitRunsEveryJob)
{
	State state;
	jobs::initialize({.worker_count = 4, .fiber_count = 16});
	main_kick_and_wait(&state);

	EXPECT_EQ(state.ran.load(), 16u);
	EXPECT_EQ(state.main_thread, std::this_thread::get_id());
	EXPECT_EQ(state.worker_in_main, 0u);
	EXPECT_EQ(worker_index(), 0u); // this thread is worker 0 until shutdown
	EXPECT_EQ(worker_count(), 4u);

	u32 elsewhere = 0;
	std::thread([&] { elsewhere = worker_index(); }).join();
	EXPECT_EQ(elsewhere, NO_WORKER);

	jobs::shutdown();
	EXPECT_EQ(worker_index(), NO_WORKER);
}

TEST(JobSystem, NestedKicksAndWaits)
{
	State state;
	jobs::initialize({.worker_count = 4, .fiber_count = 16});
	main_nested(&state);

	EXPECT_EQ(state.ran.load(), 20u);

	jobs::shutdown();
}

TEST(JobSystem, DeepFramesFitTheStack)
{
	State state;
	jobs::initialize({.worker_count = 2, .fiber_count = 8});
	main_deep(&state);

	EXPECT_TRUE(state.deep_ok);

	jobs::shutdown();
}

TEST(JobSystem, FibersAreRecycled)
{
	State state;
	jobs::initialize({.worker_count = 4, .fiber_count = 8});
	main_recycle(&state);

	EXPECT_EQ(state.ran.load(), 200u * 5u);

	jobs::shutdown();
}

// Main is whatever runs between initialize and shutdown, so it can loop as often as it likes.
TEST(JobSystem, MainRunsAgainAndAgain)
{
	State state;
	jobs::initialize({.worker_count = 4, .fiber_count = 16});
	main_nested(&state);
	main_nested(&state);

	EXPECT_EQ(state.ran.load(), 40u);
	jobs::shutdown();
}

TEST(JobSystem, KicksShareACounter)
{
	State state;
	jobs::initialize({.worker_count = 2, .fiber_count = 8});
	main_shared_counter(&state);

	EXPECT_EQ(state.ran_after_two, 8u);
	EXPECT_EQ(state.ran.load(), 10u);

	jobs::shutdown();
}

TEST(JobSystem, JobsGrowTheirParentsCounter)
{
	State state;
	jobs::initialize({.worker_count = 4, .fiber_count = 16});
	main_grow(&state);

	EXPECT_EQ(state.ran_after_two, 20u);

	jobs::shutdown();
}

TEST(JobSystem, JobsRunInParallel)
{
	State state;
	jobs::initialize({.worker_count = 4, .fiber_count = 16});
	main_parallel(&state);

	EXPECT_FALSE(state.timed_out.load());
	EXPECT_EQ(state.started.load(), 4u);

	jobs::shutdown();
}

TEST(JobSystem, JobsSpreadAcrossWorkers)
{
	State state;
	jobs::initialize({.worker_count = 4, .fiber_count = 16});
	main_spread(&state);

	EXPECT_GE(std::popcount(state.worker_mask.load()), 2);
	EXPECT_EQ(state.worker_mask.load() & ~u64{0xF}, 0u);

	jobs::shutdown();
}

TEST(JobSystem, KicksFromAnotherThread)
{
	State state;
	jobs::initialize({.worker_count = 3, .fiber_count = 16});

	std::thread producer(
		[&]
		{
			for (u32 round = 0; round < 8; ++round)
			{
				JobDef decls[8];
				fill(decls, 8, &state);

				Counter done;
				kick(decls, &done);

				while (!done.is_complete())
					std::this_thread::yield();
			}
		});

	producer.join();
	EXPECT_EQ(state.ran.load(), 64u);

	jobs::shutdown();
}

// The counter is a local of the loop body, destroyed the moment the poll sees zero, so any
// touch by the last completion after its unlock would land on the next round's counter.
TEST(JobSystem, OutsideThreadsDestroyCountersAsSoonAsTheyComplete)
{
	State state;
	jobs::initialize({.worker_count = 4, .fiber_count = 16});

	std::thread producer(
		[&]
		{
			for (u32 round = 0; round < 5000; ++round)
			{
				Counter done;
				kick({.fn = count_job, .data = &state}, &done);

				while (!done.is_complete())
				{
				}
			}
		});

	producer.join();
	EXPECT_EQ(state.ran.load(), 5000u);

	jobs::shutdown();
}

TEST(JobSystem, WaitsFromAThreadOutsideTheSystem)
{
	State state;
	jobs::initialize({.worker_count = 3, .fiber_count = 16});

	std::thread outsider(
		[&]
		{
			JobDef decls[8];
			fill(decls, 8, &state);

			Batch batch(decls);
			batch.wait(); // polls, there is no fiber to park

			Counter done;
			kick(decls, &done);
			jobs::wait(done);
		});

	outsider.join();
	EXPECT_EQ(state.ran.load(), 16u);

	jobs::shutdown();
}

TEST(JobSystem, WorkersAreNamedAndPinned)
{
	State state;
	jobs::initialize({.worker_count = 4, .fiber_count = 16, .pin_workers = true});
	main_inspect_threads(&state);

	EXPECT_GT(state.on_workers.load(), 0u);
	EXPECT_EQ(state.named_ok.load(), state.on_workers.load());
	EXPECT_EQ(state.pinned_ok.load(), state.on_workers.load());

	jobs::shutdown();
}

TEST(JobSystem, StressNestedBatches)
{
	State state;
	jobs::initialize({.worker_count = 8, .fiber_count = 52});
	main_stress(&state);

	EXPECT_EQ(state.ran.load(), 50u * 16u * 5u);

	jobs::shutdown();
}

TEST(JobSystem, Bench)
{
	Bench bench;
	jobs::initialize({.worker_count = 4, .fiber_count = 16});
	main_bench(&bench);
	std::printf("[          ] %.1f ns per one-job kick+wait, %.1f ns per job in 64-job batches, 4 workers\n",
				bench.round_trip_ns, bench.per_job_ns);

	jobs::shutdown();
}

TEST(JobSystem, ParallelForVisitsEveryIndexOnce)
{
	RangeState state;
	state.hits.resize(100000, 0);
	state.grain = 256;

	jobs::initialize({.worker_count = 4});
	main_parallel_for(&state);

	EXPECT_EQ(state.visited.load(), 100000u);
	EXPECT_EQ(state.jobs.load(), MAX_RANGE_JOBS);
	EXPECT_EQ(state.bad_index.load(), 0u);

	u32 total = 0;
	for (const u32 size : state.sizes)
		total += size;
	EXPECT_EQ(total, 100000u);

	for (const u8 hit : state.hits)
		ASSERT_EQ(hit, 1);

	jobs::shutdown();
}

TEST(JobSystem, ParallelForSplitsByGrain)
{
	RangeState state;
	state.hits.resize(10, 0);
	state.grain = 4;

	jobs::initialize({.worker_count = 4});
	main_parallel_for(&state);

	EXPECT_EQ(state.jobs.load(), 3u);
	EXPECT_EQ(state.sizes[0], 4u);
	EXPECT_EQ(state.sizes[1], 3u);
	EXPECT_EQ(state.sizes[2], 3u);
	EXPECT_EQ(state.visited.load(), 10u);

	jobs::shutdown();
}

TEST(JobSystem, ParallelForOverNothingKicksNothing)
{
	RangeState state;

	jobs::initialize({.worker_count = 2});
	main_parallel_for(&state);

	EXPECT_EQ(state.jobs.load(), 0u);

	jobs::shutdown();
}

TEST(JobSystem, ParallelForNests)
{
	RangeState state;
	state.hits.resize(8000, 0);

	jobs::initialize({.worker_count = 4});
	main_parallel_for_nested(&state);

	EXPECT_EQ(state.visited.load(), 8000u);
	for (const u8 hit : state.hits)
		ASSERT_EQ(hit, 1);

	jobs::shutdown();
}

TEST(JobSystem, MakeJobRunsACallable)
{
	State state;

	jobs::initialize({.worker_count = 2, .fiber_count = 8});
	main_make_job(&state);

	EXPECT_EQ(state.ran_after_two, 3u);

	jobs::shutdown();
}

TEST(JobSystem, ThreadLocalsFollowTheFiber)
{
	MigrationState state;

	jobs::initialize({.worker_count = 4});
	main_migration(&state);

	EXPECT_EQ(state.mismatched.load(), 0u);
	EXPECT_GT(state.migrated.load(), 0u);

	jobs::shutdown();
}

TEST(JobSystem, DefaultWorkerCountLeavesHeadroom)
{
	const u32 hardware = std::max(1u, std::thread::hardware_concurrency());

	jobs::initialize();
	EXPECT_EQ(worker_count(), hardware - std::min(1u, hardware - 1));
	jobs::shutdown();
}

TEST(JobSystem, StarvedWaitsResumeWhenFibersReturn)
{
	StallState state;

	jobs::initialize({.worker_count = 4, .fiber_count = 8});
	main_stall(&state);

	EXPECT_EQ(state.ran.load(), 1u + 7u + 28u);
	EXPECT_GE(state.stats.stalls, 3u);
	EXPECT_EQ(state.stats.parked_fibers, 0u);
	EXPECT_EQ(state.stats.queued_jobs, 0u);

	jobs::shutdown();
}

TEST(JobSystem, DetachedJobsFinishBeforeShutdown)
{
	State state;

	{
		jobs::initialize({.worker_count = 3, .fiber_count = 8});

		JobDef parents[8];
		for (JobDef& parent : parents)
			parent = {.fn = detached_parent, .data = &state};

		kick(parents);
		jobs::shutdown();
	}

	EXPECT_EQ(state.ran.load(), 8u + 32u);
}

TEST(JobSystem, ManualCountersWakeParkedFibers)
{
	SignalState state;

	jobs::initialize({.worker_count = 4});
	main_manual_counter(&state);

	EXPECT_TRUE(state.woke_after_signal);
	EXPECT_EQ(state.worker_before, 0u);
	EXPECT_EQ(state.worker_after, 0u);

	jobs::shutdown();
}

TEST(JobSystem, ManualCountersFanIn)
{
	FanInState state;

	jobs::initialize({.worker_count = 4});
	main_fan_in(&state);

	EXPECT_EQ(state.woken.load(), 8u);
	EXPECT_EQ(state.early.load(), 0u);

	jobs::shutdown();
}

TEST(JobSystemDeathTest, StuckWaitsReportTheStateAndFail)
{
	EXPECT_EXIT(
		{
			CycleState state;
			jobs::initialize({.worker_count = 2, .fiber_count = 5, .stall_report_ms = 200});
			main_cycle(&state);
			jobs::shutdown();
		},
		died_fatally, "waits on counter");
}

TEST(JobSystemDeathTest, FullJobQueueIsFatal)
{
	EXPECT_EXIT(
		{
			jobs::initialize({.worker_count = 1, .fiber_count = 4, .queue_capacity = 2});

			JobDef decls[3];
			for (JobDef& decl : decls)
				decl = {.fn = nop_job};

			kick(decls);

			jobs::shutdown();
		},
		died_fatally, "job queue full");
}

TEST(JobSystemDeathTest, WaitingWithASpinLockHeldFails)
{
#if EMBER_LOCK_TRACKING
	EXPECT_EXIT(
		{
			jobs::initialize({.worker_count = 2, .fiber_count = 5});
			main_wait_under_spin_lock(nullptr);
			jobs::shutdown();
		},
		died_fatally, "spin lock held");
#else
	GTEST_SKIP() << "lock tracking is off in this build";
#endif
}
