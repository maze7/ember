#include <ember/jobs/job_system.h>

#include <ember/containers/mpmc_queue.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/block_allocator.h>
#include <ember/sync/spin_mutex.h>
#include <ember/sync/thread.h>
#include <jobs/fiber.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace ember::jobs
{
	struct Worker;

	struct Job
	{
		JobFn fn		 = nullptr;
		void* data		 = nullptr;
		const char* name = nullptr;
		JobHandle batch; // null for a detached job
		JobStack stack = JobStack::Small;
	};

	// Scheduler side of a fiber. The seam's Fiber owns the stack and registers; this holds
	// what the loop and the counters need to know.
	struct FiberRecord
	{
		Fiber* fiber			 = nullptr;
		Worker* worker			 = nullptr; // written by whoever switches to the fiber, right before the switch
		JobStack stack			 = JobStack::Small;
		u32 pinned_worker		 = NO_WORKER;
		JobCounter* wait_counter = nullptr;
		u32 wait_generation		 = 0;
		FiberRecord* next_waiter = nullptr;
		Job handoff				 = {};				  // a large job passed over by a small fiber
		bool pool				 = false;			  // a pool fiber; thread records are not fibers to the profiler
		char name[24]			 = {};				  // persistent, the profiler keys fibers by this pointer
		const char* job_name	 = nullptr;			  // while a job runs on this fiber
		std::atomic<JobCounter*> stalled_on{nullptr}; // spinning in a wait that found no fiber; read by dumps
	};

	/**
	 * One slot of the counter pool, sized to a cache line so completing workers never share
	 * one with a neighbour. One word holds the generation and the jobs still running, so a
	 * handle resolves and a completion subtracts on the same  atomic, and a thread blocked on
	 * the word wakes when either half changes. The last completion takes the waiters under
	 * the lock and sets finalized, the owner's release sets released, and whichever of the
	 * two comes second frees the slot, which bumps the generation.
	 */
	struct alignas(EMBER_CACHE_LINE) JobCounter
	{
		std::atomic<u64> state{0};
		std::atomic<bool> locked{false};
		bool released		 = false;	// under the lock
		bool finalized		 = false;	// under the lock
		FiberRecord* waiters = nullptr; // under the lock

		void lock() noexcept;
		void unlock() noexcept;

		// True once the batch of that generation finished or the slot moved on to another one.
		[[nodiscard]] bool is_complete(u32 generation) const noexcept;

		// Blocks the calling thread on the word until is_complete would be  true.
		void wait_blocking(u32 generation) const noexcept;
	};

	static_assert(sizeof(JobCounter) == EMBER_CACHE_LINE);

	/**
	 * The batch counters. A handle names a slot and the generation it was allocated with,
	 * so a handle kept past the batch's release reads as complete instead of touching whoever
	 * reuses the slot. Allocation and release belong to the batch owner, completion to its
	 * last job, and park to a fiber that found the batch unfinished. The pool links waiting
	 * fibers and hands the ones to wake back to the scheduler.
	 */
	struct CounterPool
	{
		JobCounter* slots = nullptr;
		u32 capacity	  = 0;
		MpmcQueue<u32> free_slots{MemoryTag::Engine};

		CounterPool() noexcept = default;
		~CounterPool() noexcept;

		void init(u32 slot_count) noexcept;

		JobCounter* resolve(JobHandle handle) noexcept;
		[[nodiscard]] JobHandle allocate(u32 count) noexcept;
		void release(JobHandle handle) noexcept;
		bool is_complete(JobHandle handle) noexcept;
		[[nodiscard]] FiberRecord* complete(JobHandle batch) noexcept;
		[[nodiscard]] bool park(FiberRecord* fiber) noexcept;
		u32 live() const noexcept;

		void free_locked(JobCounter& counter) noexcept;
	};

	/**
	 * The pool fibers in their two stack classes, each with the record the scheduler and the
	 * counters read. A fiber goes back to the free list of its class when it has nothing left
	 * to run, and the lists hold every fiber, so a push never fails for lack of room.
	 */
	struct FiberPool
	{
		static constexpr u32 STACK_COUNT = static_cast<u32>(JobStack::Count);

		FiberRecord* records = nullptr; // raw storage: a record holds an atomic, so it never moves
		u32 count			 = 0;

		MpmcQueue<FiberRecord*> free_lists[STACK_COUNT] = {
			MpmcQueue<FiberRecord*>(MemoryTag::Engine),
			MpmcQueue<FiberRecord*>(MemoryTag::Engine),
		};

		FiberPool() noexcept = default;
		~FiberPool() noexcept;

		// Creates every fiber on entry, with its own record as the argument.
		void init(const JobSystemDef& def, FiberEntry entry) noexcept;

		FiberRecord* pop_free(JobStack stack) noexcept;
		void push_free(FiberRecord* fiber) noexcept;
		u32 free_count(JobStack stack) const noexcept;
	};

	// What the fiber that just switched away needs done once it is safely off its stack.
	// The fiber that receives control does it first thing.
	enum class Pending : u8
	{
		None,
		Park, // link it into its counter's wait list
		Free  // return it to the free list
	};

	/**
	 * Where idle workers sleep. A worker with nothing runnable sets its bit in the mask and
	 * waits on its own epoch; a producer that finds the bit set claims it and bumps that
	 * epoch. The two sides form a Dekker exchange: the sleeper announces, then checks for
	 * work, and  the producer publishes work, then checks the mask. Both touch the mask with
	 * read-modify-writes, so those form one total order: either the producer sees the bit
	 * and wakes the worker, or the work went up before the bit did and the check finds it.
	 */
	struct Sleepers
	{
		static constexpr u32 MAX_WORKERS = 64; // one bit each in the mask

		struct alignas(EMBER_CACHE_LINE) Epoch
		{
			std::atomic<u32> value{0};
		};

		alignas(EMBER_CACHE_LINE) std::atomic<u64> mask{0};
		Epoch epochs[MAX_WORKERS];
		u32 count = 0; // workers that may sleep, for wake_all

		// Sleeps the worker until a wake-up, unless busy() finds work after the announcement.
		template <class Busy> void sleep(u32 index, Busy&& busy) noexcept;

		void wake_one(u32 index) noexcept;
		void wake_some(u32 wanted) noexcept;
		void wake_all() noexcept;
	};

	struct Scheduler; // forward declaration

	// One worker thread. Worker 0 is the thread that calls run_main(); the others are created
	// with the system and live until it is destroyed.
	struct Worker
	{
		Scheduler* scheduler = nullptr;
		u32 index			 = 0;
		FiberRecord* current = nullptr; // fiber running on this thread
		FiberRecord* first	 = nullptr; // taken at construction, handed to the thread when it starts
		FiberRecord thread_record;		// the thread's own stack: main inside run_main(), the exit path elsewhere
		Pending pending			   = Pending::None;
		FiberRecord* pending_fiber = nullptr;
		MpmcQueue<FiberRecord*> pinned_ready{MemoryTag::Engine}; // fibers that must resume on this worker
		std::thread thread;										 // empty for worker 0
		ProfileZone segment{};									 // the fiber this thread is running, on its own track
	};

	struct Scheduler
	{
		static constexpr u32 PRIORITY_COUNT = static_cast<u32>(JobPriority::Count);
		static constexpr u32 IDLE_SPINS		= 128;

		JobSystemDef def;
		bool running = false;
		std::atomic<bool> stopping{false};

		CounterPool counters;

		FiberPool fibers;
		MpmcQueue<FiberRecord*> ready{MemoryTag::Engine};

		Worker* workers	 = nullptr;
		u32 worker_count = 0;
		Sleepers sleepers;

		MpmcQueue<Job> jobs[PRIORITY_COUNT] = {MpmcQueue<Job>(MemoryTag::Engine), MpmcQueue<Job>(MemoryTag::Engine),
											   MpmcQueue<Job>(MemoryTag::Engine)};

		std::atomic<u32> parked{0};		  // fibers linked into a wait list
		std::atomic<u64> stalls{0};		  // waits that found no fiber
		std::atomic<bool> failing{false}; // a stalled worker is dumping and failing

		explicit Scheduler(const JobSystemDef& def) noexcept;
		~Scheduler() noexcept;

		void park(FiberRecord* fiber) noexcept;
		void complete(JobHandle batch) noexcept;

		FiberRecord* pop_ready(Worker& worker) noexcept;
		FiberRecord* pop_fiber(Worker& worker) noexcept;
		void make_ready(FiberRecord* fiber) noexcept;
		void switch_to(Worker& worker, FiberRecord* from, FiberRecord* to) noexcept;
		void finish_switch(Worker& worker) noexcept;
		Worker* yield_to(FiberRecord* self, FiberRecord* next, Pending action) noexcept;

		void worker_main(Worker& worker) noexcept;
		bool has_work(const Worker& worker) const noexcept;
		void idle(Worker& worker) noexcept;

		void submit(Span<const JobDef> jobs, JobHandle batch) noexcept;
		bool pop_job(Job& job) noexcept;
		void run_job(FiberRecord* self, const Job& job) noexcept;
		void loop(FiberRecord* self) noexcept;
		void wait(JobHandle handle) noexcept;
		FiberRecord* stall(FiberRecord* self, JobCounter& counter, JobHandle handle) noexcept;
		FiberRecord* wait_for_large_fiber() noexcept;
		void spin_or_fail(u32& spin, std::chrono::steady_clock::time_point since, const char* reason) noexcept;

		void dump(const char* reason) noexcept;
		const char* fiber_label(const FiberRecord& fiber) const noexcept;
		JobStats stats() const noexcept;
	};

	namespace
	{
		// The only thread local in the system. Nothing caches its result across a switch:
		// after a wait a fiber reads its worker from its own record instead.
		thread_local Worker* t_worker = nullptr;

		Scheduler* s_scheduler = nullptr;

		// The one way the entry points reach the scheduler.
		Scheduler& scheduler() noexcept
		{
			EMBER_ASSERT(s_scheduler != nullptr && "no job system");
			return *s_scheduler;
		}

		EMBER_NOINLINE Worker* current_worker() noexcept { return t_worker; }

		void fiber_main(void* arg)
		{
			auto* self = static_cast<FiberRecord*>(arg);
			self->worker->scheduler->loop(self);
		}

		[[nodiscard]] size_t queue_capacity(u32 count) noexcept { return std::bit_ceil(count < 2 ? 2u : count); }

		[[nodiscard]] constexpr u64 pack_state(u32 generation, u32 remaining) noexcept
		{
			return (u64{generation} << 32) | remaining;
		}

		[[nodiscard]] constexpr u32 generation_of(u64 state) noexcept { return static_cast<u32>(state >> 32); }
		[[nodiscard]] constexpr u32 remaining_of(u64 state) noexcept { return static_cast<u32>(state); }

		// Running out of a pool is a configuration error, so it ends the process the way an
		// allocation failure does, in every build.
		[[noreturn]] void fail(const char* what) noexcept
		{
			EMBER_ERROR("(ember::jobs) {}", what);
			EMBER_DEBUG_BREAK();
			std::abort();
		}

		// A push fails because the queue is full or because a pop of the target cell is mid
		// flight on another thread and has not released it yet. The hint tells the two apart,
		// and the second clears as soon as that pop finishes.
		template <class T> [[nodiscard]] bool push_or_full(MpmcQueue<T>& queue, const T& value) noexcept
		{
			for (u32 spins = 0;; detail::cpu_relax(spins++))
			{
				if (queue.try_push(value))
					return true;

				if (queue.size_hint() >= queue.capacity())
					return false;
			}
		}

		// Pools never fill: an object is pushed only by the holder that took it out.
		template <class T> void push_held(MpmcQueue<T>& queue, const T& value) noexcept
		{
			const bool pushed = push_or_full(queue, value);
			EMBER_ASSERT(pushed && "pool queue sized for every object");
			(void)pushed;
		}

		// A pop deciding that a pool is empty must not trust one failed attempt while a push
		// is mid flight.
		template <class T> [[nodiscard]] bool pop_any(MpmcQueue<T>& queue, T& out) noexcept
		{
			for (u32 spins = 0;; detail::cpu_relax(spins++))
			{
				if (queue.try_pop(out))
					return true;

				if (queue.size_hint() == 0)
					return false;
			}
		}

		// A segment is a zone on the thread's own track for the time the thread spends running
		// one fiber, named after the job on it: fiber tracks show a job whole, thread tracks
		// show where each piece of it ran. The profiler files every event under the fiber it
		// has entered, so the segment opens and closes in thread context: begin it before
		// entering the fiber, leave the fiber before ending it. Thread records are the thread
		// itself to the profiler and get neither.
		void trace_enter(Worker& worker, const FiberRecord* fiber) noexcept
		{
			if (!fiber->pool)
				return;

#if EMBER_USE_TRACY
			EMBER_PROFILE_ZONE_BEGIN(worker.segment, "fiber");

			const char* label = fiber->job_name != nullptr ? fiber->job_name : fiber->name;
			EMBER_PROFILE_ZONE_RENAME(worker.segment, label, std::strlen(label));
#else
			(void)worker;
#endif
			EMBER_PROFILE_FIBER_ENTER(fiber->name);
		}

		void trace_leave(Worker& worker, const FiberRecord* fiber) noexcept
		{
			if (!fiber->pool)
				return;

			EMBER_PROFILE_FIBER_LEAVE();
			EMBER_PROFILE_ZONE_END(worker.segment);
			(void)worker;
		}

		// Splits the segment at a job boundary, so the thread track shows the job as its own piece.
		void trace_split(const FiberRecord* fiber) noexcept
		{
			Worker& worker = *fiber->worker;
			trace_leave(worker, fiber);
			trace_enter(worker, fiber);
		}
	}

	Scheduler::Scheduler(const JobSystemDef& def) noexcept : def(def)
	{
		const u32 hardware = std::max(1u, std::thread::hardware_concurrency());

		worker_count =
			def.worker_count != 0 ? def.worker_count : hardware - std::min(def.reserved_threads, hardware - 1);
		worker_count   = std::min(worker_count, Sleepers::MAX_WORKERS);
		sleepers.count = worker_count;

		fibers.init(def, fiber_main);
		EMBER_ASSERT(fibers.count >= worker_count && "every worker thread needs a fiber to run its loop");

		ready.init(queue_capacity(fibers.count + worker_count));

		for (MpmcQueue<Job>& queue : jobs)
			queue.init(queue_capacity(def.queue_capacity));

		counters.init(def.counter_capacity < 1 ? 1 : def.counter_capacity);
		workers = static_cast<Worker*>(
			memory::heap(MemoryTag::Engine).allocate(worker_count * sizeof(Worker), alignof(Worker)));

		for (u32 index = 0; index < worker_count; ++index)
		{
			Worker& worker = *std::construct_at(workers + index);

			worker.scheduler				   = this;
			worker.index					   = index;
			worker.thread_record.stack		   = JobStack::Large;
			worker.thread_record.pinned_worker = index;
			worker.pinned_ready.init(queue_capacity(fibers.count + worker_count));
		}

		// Threads start last, once every queue and fiber they may touch exists. Each thread's
		// first fiber is taken here, ahead of a main that may drain the pool before the
		// thread gets to run.
		for (u32 index = 1; index < worker_count; ++index)
		{
			Worker& worker = workers[index];

			worker.first = fibers.pop_free(JobStack::Small);
			if (worker.first == nullptr)
				worker.first = fibers.pop_free(JobStack::Large);
			EMBER_ASSERT(worker.first != nullptr && "out of fibers");

			worker.thread = std::thread([this, index] { worker_main(workers[index]); });
		}
	}

	Scheduler::~Scheduler() noexcept
	{
		stopping.store(true, std::memory_order_release);
		sleepers.wake_all();

		for (u32 index = 1; index < worker_count; ++index)
			if (workers[index].thread.joinable())
				workers[index].thread.join();

		const JobStats snapshot = stats();
		EMBER_ASSERT(snapshot.free_small_fibers + snapshot.free_large_fibers == fibers.count &&
					 "fibers still parked at shutdown");
		EMBER_ASSERT(snapshot.ready_fibers == 0 && "fibers still ready at shutdown");
		EMBER_ASSERT(snapshot.live_batches == 0 && "batches still owned at shutdown");
		EMBER_ASSERT(snapshot.queued_jobs == 0 && "jobs still queued at shutdown");
		(void)snapshot;

		for (u32 index = 0; index < worker_count; ++index)
			std::destroy_at(workers + index);

		memory::heap(MemoryTag::Engine).deallocate(workers, worker_count * sizeof(Worker), alignof(Worker));
	}

	// Test before the exchange, so a spinner reads the line shared instead of bouncing it.
	void JobCounter::lock() noexcept
	{
		for (u32 spins = 0;; detail::cpu_relax(spins++))
		{
			if (locked.load(std::memory_order_relaxed))
				continue;

			if (!locked.exchange(true, std::memory_order_acquire))
				return;
		}
	}

	void JobCounter::unlock() noexcept { locked.store(false, std::memory_order_release); }

	bool JobCounter::is_complete(u32 generation) const noexcept
	{
		const u64 current = state.load(std::memory_order_acquire);
		return generation_of(current) != generation || remaining_of(current) == 0;
	}

	// A reuse of the slot changes the same word, so a stale sleeper wakes and leaves.
	void JobCounter::wait_blocking(u32 generation) const noexcept
	{
		u64 current = state.load(std::memory_order_acquire);

		while (generation_of(current) == generation && remaining_of(current) != 0)
		{
			state.wait(current, std::memory_order_acquire);
			current = state.load(std::memory_order_acquire);
		}
	}

	CounterPool::~CounterPool() noexcept
	{
		if (slots == nullptr)
			return;

		for (u32 index = 0; index < capacity; ++index)
			std::destroy_at(slots + index);

		memory::heap(MemoryTag::Engine).deallocate(slots, capacity * sizeof(JobCounter), alignof(JobCounter));
	}

	void CounterPool::init(u32 slot_count) noexcept
	{
		EMBER_ASSERT(slots == nullptr && "init runs once");
		EMBER_ASSERT(slot_count != 0);

		capacity = slot_count;
		slots	 = static_cast<JobCounter*>(
			memory::heap(MemoryTag::Engine).allocate(capacity * sizeof(JobCounter), alignof(JobCounter)));
		free_slots.init(queue_capacity(capacity));

		for (u32 index = 0; index < capacity; ++index)
		{
			JobCounter& counter = *std::construct_at(slots + index);
			counter.state.store(pack_state(1, 0), std::memory_order_relaxed);
			push_held(free_slots, index);
		}
	}

	JobCounter* CounterPool::resolve(JobHandle handle) noexcept
	{
		if (handle.is_null() || handle.index >= capacity)
			return nullptr;

		return slots + handle.index;
	}

	// The slot is nobody's until the handle is returned, so plain stores suffice; the count
	// is in place before any job can subtract from it.
	JobHandle CounterPool::allocate(u32 count) noexcept
	{
		u32 index = 0;

		if (!pop_any(free_slots, index))
			fail("counter pool exhausted, raise JobSystemDef::counter_capacity or release batches sooner");

		JobCounter& counter	 = slots[index];
		const u32 generation = generation_of(counter.state.load(std::memory_order_relaxed));

		EMBER_ASSERT(remaining_of(counter.state.load(std::memory_order_relaxed)) == 0);
		EMBER_ASSERT(counter.waiters == nullptr);

		counter.released  = false;
		counter.finalized = false;
		counter.state.store(pack_state(generation, count), std::memory_order_release);

		return {index, generation};
	}

	// Under the lock, which it releases. A new generation makes every handle to the old batch
	// read as complete, then the slot goes back to the pool.
	void CounterPool::free_locked(JobCounter& counter) noexcept
	{
		EMBER_ASSERT(counter.released && counter.finalized && counter.waiters == nullptr);

		u32 generation = generation_of(counter.state.load(std::memory_order_relaxed)) + 1;
		if (generation == 0)
			generation = 1;

		counter.state.store(pack_state(generation, 0), std::memory_order_release);
		counter.unlock();

		push_held(free_slots, static_cast<u32>(&counter - slots));
	}

	void CounterPool::release(JobHandle handle) noexcept
	{
		JobCounter* counter = resolve(handle);
		EMBER_ASSERT(counter != nullptr && "release of a null or foreign batch");

		if (counter == nullptr)
			return;

		counter->lock();

		if (generation_of(counter->state.load(std::memory_order_relaxed)) != handle.generation || counter->released)
		{
			counter->unlock();
			EMBER_ASSERT(false && "batch released twice");
			return;
		}

		counter->released = true;

		if (counter->finalized)
			free_locked(*counter);
		else
			counter->unlock();
	}

	bool CounterPool::is_complete(JobHandle handle) noexcept
	{
		const JobCounter* counter = resolve(handle);
		return counter == nullptr || counter->is_complete(handle.generation);
	}

	// Returns the fibers the last completion found waiting, linked through next_waiter, for the
	// caller to wake. Threads blocked on the word wake here; parked fibers are taken under the
	// lock, and the owner's release may already be waiting for finalized.
	FiberRecord* CounterPool::complete(JobHandle batch) noexcept
	{
		if (batch.is_null())
			return nullptr;

		JobCounter& counter = slots[batch.index];
		const u64 previous	= counter.state.fetch_sub(1, std::memory_order_acq_rel);

		EMBER_ASSERT(generation_of(previous) == batch.generation && remaining_of(previous) != 0);

		if (remaining_of(previous) != 1)
			return nullptr;

		counter.state.notify_all();
		counter.lock();

		FiberRecord* wake = counter.waiters;
		counter.waiters	  = nullptr;
		counter.finalized = true;

		if (counter.released)
			free_locked(counter);
		else
			counter.unlock();

		return wake;
	}

	// Links the fiber into its counter's wait list, or returns false when the batch already
	// finished. Under the lock the last completion has either zeroed the word already, which
	// this check sees, or has not taken the list yet, in which case it finds the link.
	bool CounterPool::park(FiberRecord* fiber) noexcept
	{
		JobCounter& counter = *fiber->wait_counter;

		counter.lock();

		if (counter.is_complete(fiber->wait_generation))
		{
			counter.unlock();
			return false;
		}

		fiber->next_waiter = counter.waiters;
		counter.waiters	   = fiber;

		counter.unlock();
		return true;
	}

	u32 CounterPool::live() const noexcept { return capacity - static_cast<u32>(free_slots.size_hint()); }

	// Runs on the fiber that took over from the parking one, so the parked context is complete
	// before anything can resume it.
	void Scheduler::park(FiberRecord* fiber) noexcept
	{
		parked.fetch_add(1, std::memory_order_relaxed);

		if (counters.park(fiber))
			return;

		parked.fetch_sub(1, std::memory_order_relaxed);
		make_ready(fiber);
	}

	void Scheduler::complete(JobHandle batch) noexcept
	{
		FiberRecord* wake = counters.complete(batch);

		while (wake != nullptr)
		{
			FiberRecord* next = wake->next_waiter;
			parked.fetch_sub(1, std::memory_order_relaxed);
			make_ready(wake);
			wake = next;
		}
	}

	FiberPool::~FiberPool() noexcept
	{
		if (records == nullptr)
			return;

		for (u32 index = 0; index < count; ++index)
		{
			fiber_destroy(records[index].fiber);
			std::destroy_at(records + index);
		}

		memory::heap(MemoryTag::Engine).deallocate(records, count * sizeof(FiberRecord), alignof(FiberRecord));
	}

	void FiberPool::init(const JobSystemDef& def, FiberEntry entry) noexcept
	{
		EMBER_ASSERT(records == nullptr && "init runs once");

		count	= def.small_fibers + def.large_fibers;
		records = static_cast<FiberRecord*>(
			memory::heap(MemoryTag::Engine).allocate(count * sizeof(FiberRecord), alignof(FiberRecord)));

		free_lists[static_cast<u32>(JobStack::Small)].init(queue_capacity(def.small_fibers));
		free_lists[static_cast<u32>(JobStack::Large)].init(queue_capacity(def.large_fibers));

		for (u32 index = 0; index < count; ++index)
		{
			FiberRecord& record = *std::construct_at(records + index);
			record.stack		= index < def.small_fibers ? JobStack::Small : JobStack::Large;
			record.pool			= true;
			std::snprintf(record.name, sizeof(record.name), "fiber %u", index);

			const size_t stack_size = record.stack == JobStack::Small ? def.small_stack : def.large_stack;
			record.fiber			= fiber_create({.stack_size = stack_size, .entry = entry, .arg = &record});
			EMBER_ASSERT(record.fiber != nullptr);

			push_free(&record);
		}
	}

	FiberRecord* FiberPool::pop_free(JobStack stack) noexcept
	{
		FiberRecord* fiber = nullptr;
		return free_lists[static_cast<u32>(stack)].try_pop(fiber) ? fiber : nullptr;
	}

	void FiberPool::push_free(FiberRecord* fiber) noexcept
	{
		push_held(free_lists[static_cast<u32>(fiber->stack)], fiber);
	}

	u32 FiberPool::free_count(JobStack stack) const noexcept
	{
		return static_cast<u32>(free_lists[static_cast<u32>(stack)].size_hint());
	}

	FiberRecord* Scheduler::pop_ready(Worker& worker) noexcept
	{
		FiberRecord* fiber = nullptr;

		if (worker.pinned_ready.try_pop(fiber))
			return fiber;

		return ready.try_pop(fiber) ? fiber : nullptr;
	}

	// Anything this worker may continue on: a fiber whose wait completed, else a fresh one.
	FiberRecord* Scheduler::pop_fiber(Worker& worker) noexcept
	{
		FiberRecord* next = pop_ready(worker);

		if (next == nullptr)
			next = fibers.pop_free(JobStack::Small);

		if (next == nullptr)
			next = fibers.pop_free(JobStack::Large);

		return next;
	}

	void Scheduler::make_ready(FiberRecord* fiber) noexcept
	{
		if (fiber->pinned_worker != NO_WORKER)
		{
			Worker& worker = workers[fiber->pinned_worker];
			push_held(worker.pinned_ready, fiber);
			sleepers.wake_one(worker.index);
			return;
		}

		push_held(ready, fiber);
		sleepers.wake_some(1);
	}

	// Returns when from is resumed, on whatever thread resumes it.
	void Scheduler::switch_to(Worker& worker, FiberRecord* from, FiberRecord* to) noexcept
	{
		EMBER_ASSERT(from != to);
		to->worker	   = &worker;
		worker.current = to;

		trace_leave(worker, from);
		fiber_switch(from->fiber, to->fiber);
		trace_enter(*from->worker, from);
	}

	void Scheduler::finish_switch(Worker& worker) noexcept
	{
		FiberRecord* fiber	 = worker.pending_fiber;
		const Pending action = worker.pending;

		worker.pending		 = Pending::None;
		worker.pending_fiber = nullptr;

		if (action == Pending::Park)
			park(fiber);
		else if (action == Pending::Free)
			fibers.push_free(fiber);
	}

	/**
	 * Switches to next with an action for whoever  takes over, and once something
	 * switches back finishes the action that fiber left. Returns the worker this fiber
	 * resumed on.
	 */
	Worker* Scheduler::yield_to(FiberRecord* self, FiberRecord* next, Pending action) noexcept
	{
		Worker& worker = *self->worker;

		worker.pending		 = action;
		worker.pending_fiber = self;
		switch_to(worker, self, next);

		Worker* resumed = self->worker;
		finish_switch(*resumed);
		return resumed;
	}

	// Worker threads live here. The thread's own stack becomes its thread record, used only
	// to leave the loop at shutdown; everything else runs on pool fibers.
	void Scheduler::worker_main(Worker& worker) noexcept
	{
		memory::initialize_thread();
		BlockAllocator::register_thread();
		t_worker = &worker;

		char name[16];
		std::snprintf(name, sizeof(name), "ember.jobs.%u", worker.index);
		set_thread_name(name);
		EMBER_PROFILE_THREAD(name);

		if (def.pin_workers)
			(void)set_thread_affinity(worker.index % std::max(1u, std::thread::hardware_concurrency()));

		worker.thread_record.fiber	= fiber_adopt_thread();
		worker.thread_record.worker = &worker;
		worker.current				= &worker.thread_record;

		FiberRecord* first = worker.first;
		worker.first	   = nullptr;

		switch_to(worker, &worker.thread_record, first);

		finish_switch(worker);
		fiber_release_thread(worker.thread_record.fiber);
		worker.thread_record.fiber = nullptr;
		t_worker				   = nullptr;
		BlockAllocator::unregister_thread();
		memory::shutdown_thread();
	}

	bool Scheduler::has_work(const Worker& worker) const noexcept
	{
		if (worker.pinned_ready.size_hint() != 0 || ready.size_hint() != 0)
			return true;

		for (const MpmcQueue<Job>& queue : jobs)
			if (queue.size_hint() != 0)
				return true;

		return false;
	}

	/**
	 * The epoch is read before the annoucement, so a wake-up landing between the announcement
	 * and the wait shows up as a changed value and the wait returns at once.
	 */
	template <class Busy> void Sleepers::sleep(u32 index, Busy&& busy) noexcept
	{
		const u32 observed = epochs[index].value.load(std::memory_order_acquire);
		const u64 bit	   = u64{1} << index;

		mask.fetch_or(bit, std::memory_order_acq_rel);

		if (!busy())
			epochs[index].value.wait(observed, std::memory_order_acquire);

		mask.fetch_and(~bit, std::memory_order_relaxed);
	}

	/**
	 * Producers ovserve the mask with a read-modify-write rather than a load: that orders
	 * it after the work they just published, against the sleeper's own read-modify-write.
	 */
	void Sleepers::wake_one(u32 index) noexcept
	{
		const u64 bit = u64{1} << index;

		if ((mask.fetch_or(0, std::memory_order_acq_rel) & bit) == 0)
			return;

		if ((mask.fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0)
			return;

		epochs[index].value.fetch_add(1, std::memory_order_release);
		epochs[index].value.notify_one();
	}

	/** Claims sleepers one bit at a time so several producers do not all wake the same one */
	void Sleepers::wake_some(u32 wanted) noexcept
	{
		u64 sleeping = mask.fetch_or(0, std::memory_order_acq_rel);

		while (sleeping != 0 && wanted != 0)
		{
			const u32 index = static_cast<u32>(std::countr_zero(sleeping));
			const u64 bit	= u64{1} << index;
			sleeping &= ~bit;

			if ((mask.fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0)
				continue;

			epochs[index].value.fetch_add(1, std::memory_order_release);
			epochs[index].value.notify_one();
			--wanted;
		}
	}

	void Sleepers::wake_all() noexcept
	{
		for (u32 index = 0; index < count; ++index)
		{
			epochs[index].value.fetch_add(1, std::memory_order_release);
			epochs[index].value.notify_all();
		}
	}

	// Spin a little for work that is about to arrive, then sleep.
	void Scheduler::idle(Worker& worker) noexcept
	{
		for (u32 spin = 0; spin < IDLE_SPINS; ++spin)
		{
			if (has_work(worker) || stopping.load(std::memory_order_relaxed))
				return;

			detail::cpu_relax(spin);
		}

		trace_leave(worker, worker.current);

		const auto busy = [&]() noexcept { return has_work(worker) || stopping.load(std::memory_order_acquire); };
		sleepers.sleep(worker.index, busy);

		trace_enter(worker, worker.current);
	}
	void Scheduler::submit(Span<const JobDef> defs, JobHandle batch) noexcept
	{
		for (const JobDef& job : defs)
		{
			EMBER_ASSERT(job.fn != nullptr);
			EMBER_ASSERT((job.stack != JobStack::Large || def.large_fibers != 0) && "no large fibers configured");

			const bool pushed =
				push_or_full(jobs[static_cast<u32>(job.priority)],
							 Job{.fn = job.fn, .data = job.data, .name = job.name, .batch = batch, .stack = job.stack});

			if (!pushed)
				fail("job queue full, raise JobSystemDef::queue_capacity");
		}

		sleepers.wake_some(static_cast<u32>(defs.size()));
	}

	bool Scheduler::pop_job(Job& job) noexcept
	{
		for (MpmcQueue<Job>& queue : jobs)
			if (queue.try_pop(job))
				return true;

		return false;
	}

	void Scheduler::run_job(FiberRecord* self, const Job& job) noexcept
	{
		self->job_name = job.name != nullptr ? job.name : "job";
		trace_split(self);

		{
			EMBER_PROFILE_SCOPE("job");
			EMBER_PROFILE_ZONE_NAME(self->job_name, std::strlen(self->job_name));
			job.fn(job.data);
		}

		self->job_name = nullptr;
		trace_split(self);

		complete(job.batch);
	}

	/**
	 * Every pool fiber runs this forever.
	 *
	 * Ready fibers come first because they hold a stack and an unfinished job;
	 * new jobs start only when nothing is waiting to resume. A large fiber runs
	 * any job. A small fiber hands a large job to a large fiber, which runs it
	 * first thing. A fiber that switches to another one has nothing left to do
	 * and frees itself in the process.
	 */
	void Scheduler::loop(FiberRecord* self) noexcept
	{
		Worker* worker = self->worker;
		trace_enter(*worker, self);
		finish_switch(*worker);

		for (;;)
		{
			if (self->handoff.fn != nullptr)
			{
				const Job job = self->handoff;
				self->handoff = {};
				run_job(self, job);
				worker = self->worker;
				continue;
			}

			if (FiberRecord* next = pop_ready(*worker))
			{
				worker = yield_to(self, next, Pending::Free);
				continue;
			}

			Job job;

			if (pop_job(job))
			{
				if (job.stack == JobStack::Large && self->stack == JobStack::Small)
				{
					FiberRecord* target = wait_for_large_fiber();
					target->handoff		= job;
					worker				= yield_to(self, target, Pending::Free);
					continue;
				}

				run_job(self, job);
				worker = self->worker;
				continue;
			}

			if (stopping.load(std::memory_order_acquire))
			{
				yield_to(self, &worker->thread_record, Pending::Free);
				EMBER_UNREACHABLE_ASSERT();
			}

			idle(*worker);
		}
	}

	void Scheduler::wait(JobHandle handle) noexcept
	{
		JobCounter* counter = counters.resolve(handle);
		if (counter == nullptr || counter->is_complete(handle.generation))
			return;

		Worker* worker = current_worker();

		if (worker == nullptr)
		{
			// Threads outside the system have no fiber to park, so they block on the word.
			counter->wait_blocking(handle.generation);
			return;
		}

		FiberRecord* self = worker->current;
		FiberRecord* next = pop_fiber(*worker);

		if (next == nullptr)
		{
			next = stall(self, *counter, handle);
			if (next == nullptr)
				return;
		}

		self->wait_counter	  = counter;
		self->wait_generation = handle.generation;

		yield_to(self, next, Pending::Park);
	}

	/**
	 * Every fiber is parked or busy on another worker. The wait spins for one to come back or
	 * for its own batch  to finish, and after stall_report_ms of neither the pool is too small
	 * for the job graph: the dump names the batches holding it.
	 */
	FiberRecord* Scheduler::stall(FiberRecord* self, JobCounter& counter, JobHandle handle) noexcept
	{
		EMBER_PROFILE_SCOPE_C("stalled wait", PROFILE_COLOR_WAIT);
		stalls.fetch_add(1, std::memory_order_relaxed);
		self->stalled_on.store(&counter, std::memory_order_relaxed);

		Worker& worker	  = *self->worker;
		const auto since  = std::chrono::steady_clock::now();
		FiberRecord* next = nullptr;

		for (u32 spin = 0;;)
		{
			if (counter.is_complete(handle.generation))
				break;

			next = pop_fiber(worker);
			if (next != nullptr)
				break;

			spin_or_fail(spin, since, "a wait found no fiber and nothing finished");
		}

		self->stalled_on.store(nullptr, std::memory_order_relaxed);
		return next;
	}

	/**
	 * A small fiber holding a large job. Large fibers come back as large jobs finish;
	 * none for stall_report_ms means the large pool is too small for the job graph.
	 */
	FiberRecord* Scheduler::wait_for_large_fiber() noexcept
	{
		const auto since = std::chrono::steady_clock::now();

		for (u32 spin = 0;;)
		{
			if (FiberRecord* fiber = fibers.pop_free(JobStack::Large))
				return fiber;

			spin_or_fail(spin, since, "a large job found no large fiber");
		}
	}

	// One turn of a spin that may not end: the clock is read every 1024 turns, and a spin
	// older than stall_report_ms logs the state and fails. The first worker there reports;
	// the others keep spinning for the moment the failure takes.
	void Scheduler::spin_or_fail(u32& spin, std::chrono::steady_clock::time_point since, const char* reason) noexcept
	{
		if (def.stall_report_ms != 0 && (spin & 1023) == 1023 &&
			std::chrono::steady_clock::now() - since >= std::chrono::milliseconds(def.stall_report_ms) &&
			!failing.exchange(true, std::memory_order_acq_rel))
		{
			dump(reason);
			fail(reason);
		}

		detail::cpu_relax(spin++);
	}

	void Scheduler::dump(const char* reason) noexcept
	{
		char line[512];

		const JobStats snapshot = stats();

		std::snprintf(line, sizeof(line),
					  "%s: %u small and %u large fibers free, %u parked, %u ready, %u jobs queued, %u batches live",
					  reason, snapshot.free_small_fibers, snapshot.free_large_fibers, snapshot.parked_fibers,
					  snapshot.ready_fibers, snapshot.queued_jobs, snapshot.live_batches);
		EMBER_ERROR("(ember::jobs) {}", line);

		for (u32 index = 0; index < counters.capacity; ++index)
		{
			JobCounter& counter = counters.slots[index];
			counter.lock();

			const u32 remaining = remaining_of(counter.state.load(std::memory_order_relaxed));
			FiberRecord* waiter = counter.waiters;

			if (remaining == 0 && waiter == nullptr)
			{
				counter.unlock();
				continue;
			}

			int used = std::snprintf(line, sizeof(line), "  batch %u: %u jobs left", index, remaining);

			for (; waiter != nullptr && used < static_cast<int>(sizeof(line)); waiter = waiter->next_waiter)
				used += std::snprintf(line + used, sizeof(line) - used, ", %s parked", fiber_label(*waiter));

			counter.unlock();

			// Stalled waits sit on no list; their fibers name the counter. Read racily, this
			// is a diagnostic.
			const auto add_stalled = [&](const FiberRecord& fiber)
			{
				if (fiber.stalled_on.load(std::memory_order_relaxed) != &counter ||
					used >= static_cast<int>(sizeof(line)))
					return;

				used += std::snprintf(line + used, sizeof(line) - used, ", %s stalled", fiber_label(fiber));
			};

			for (u32 fiber = 0; fiber < fibers.count; ++fiber)
				add_stalled(fibers.records[fiber]);
			for (u32 worker = 0; worker < worker_count; ++worker)
				add_stalled(workers[worker].thread_record);

			EMBER_ERROR("(ember::jobs) {}", line);
		}
	}

	const char* Scheduler::fiber_label(const FiberRecord& fiber) const noexcept
	{
		if (&fiber == &workers[0].thread_record)
			return "main";

		return fiber.job_name != nullptr ? fiber.job_name : fiber.name;
	}

	JobStats Scheduler::stats() const noexcept
	{
		size_t queued = 0;
		for (const MpmcQueue<Job>& queue : jobs)
			queued += queue.size_hint();

		size_t ready_count = ready.size_hint();
		for (u32 index = 0; index < worker_count; ++index)
			ready_count += workers[index].pinned_ready.size_hint();

		return {
			.free_small_fibers = fibers.free_count(JobStack::Small),
			.free_large_fibers = fibers.free_count(JobStack::Large),
			.parked_fibers	   = parked.load(std::memory_order_relaxed),
			.ready_fibers	   = static_cast<u32>(ready_count),
			.queued_jobs	   = static_cast<u32>(queued),
			.live_batches	   = counters.live(),
			.stalls			   = stalls.load(std::memory_order_relaxed),
		};
	}

	void initialize(const JobSystemDef& def) noexcept
	{
		EMBER_ASSERT(s_scheduler == nullptr && "one job system per process");
		s_scheduler = memory::new_object<Scheduler>(MemoryTag::Engine, def);
	}

	void shutdown() noexcept
	{
		EMBER_ASSERT(!scheduler().running && "shutdown inside run_main");
		memory::delete_object(MemoryTag::Engine, s_scheduler);
		s_scheduler = nullptr;
	}

	void run_main(JobFn main, void* data) noexcept
	{
		Scheduler& system = scheduler();
		EMBER_ASSERT(!system.running && "run_main is not re-entrant");
		system.running = true;

		Worker& worker				= system.workers[0];
		worker.thread_record.fiber	= fiber_adopt_thread();
		worker.thread_record.worker = &worker;
		worker.current				= &worker.thread_record;
		t_worker					= &worker;

		main(data);

		EMBER_ASSERT(worker.current == &worker.thread_record);

		t_worker = nullptr;
		fiber_release_thread(worker.thread_record.fiber);
		worker.thread_record.fiber = nullptr;
		system.running			   = false;
	}

	JobHandle submit(Span<const JobDef> jobs) noexcept
	{
		if (jobs.empty())
			return {};

		Scheduler& system	   = scheduler();
		const JobHandle handle = system.counters.allocate(static_cast<u32>(jobs.size()));
		system.submit(jobs, handle);
		return handle;
	}

	void submit_detached(Span<const JobDef> jobs) noexcept
	{
		if (jobs.empty())
			return;

		scheduler().submit(jobs, {});
	}

	void wait(JobHandle batch) noexcept { scheduler().wait(batch); }

	bool is_complete(JobHandle batch) noexcept { return scheduler().counters.is_complete(batch); }

	void release(JobHandle batch) noexcept { scheduler().counters.release(batch); }

	u32 worker_count() noexcept { return scheduler().worker_count; }

	u32 worker_index() noexcept
	{
		const Worker* worker = current_worker();
		return worker != nullptr ? worker->index : NO_WORKER;
	}

	JobStats stats() noexcept { return scheduler().stats(); }

	void dump_state() noexcept { scheduler().dump("job system state"); }

	namespace
	{
		struct RangeJob
		{
			JobRange range;
			RangeFn fn;
			void* data;
		};

		void run_range(void* data)
		{
			const RangeJob& job = *static_cast<const RangeJob*>(data);
			job.fn(job.range, job.data);
		}
	}

	void parallel_for(const ParallelForDef& def, RangeFn fn, void* data) noexcept
	{
		EMBER_ASSERT(fn != nullptr);

		if (def.count == 0)
			return;

		const u32 grain	   = def.grain == 0 ? 1 : def.grain;
		const u32 jobs	   = std::min(1 + (def.count - 1) / grain, MAX_RANGE_JOBS);
		const u32 per_job  = def.count / jobs;
		const u32 leftover = def.count % jobs; // the first leftover jobs take one extra item

		RangeJob ranges[MAX_RANGE_JOBS];
		JobDef defs[MAX_RANGE_JOBS];

		u32 begin = 0;
		for (u32 i = 0; i < jobs; ++i)
		{
			const u32 end = begin + per_job + (i < leftover ? 1 : 0);

			ranges[i] = {.range = {begin, end, i}, .fn = fn, .data = data};
			defs[i]	  = {
				.fn		  = run_range,
				.data	  = &ranges[i],
				.name	  = def.name,
				.priority = def.priority,
				.stack	  = def.stack,
			};

			begin = end;
		}

		const JobHandle batch = submit(Span<const JobDef>(defs, jobs));
		wait(batch);
		release(batch);
	}
}
