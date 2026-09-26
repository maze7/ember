#pragma once

#include <ember/jobs/job_system.h>

#include <ember/containers/mpmc_queue.h>
#include <ember/core/common.h>
#include <ember/core/profile.h>
#include <ember/sync/spin_mutex.h>
#include <jobs/fiber.h>

#include <atomic>
#include <chrono>
#include <semaphore>
#include <thread>

/**
 * The scheduler behind the jobs API: fiber records and the fiber pool, the sleep protocol,
 * the workers and the  loop. Private to the module.
 */
namespace ember::jobs
{
	struct Worker;

	/**
	 * A queued job: the definition minus the priority, which the queue implies, plus the
	 * counter it completes. Null for fire and forget.
	 */
	struct Job
	{
		JobFn fn		 = nullptr;
		void* data		 = nullptr;
		const char* name = nullptr;
		Counter* counter = nullptr;
	};

	/**
	 * A push fails because the queue is full or because a pop of the target cell is mid flight
	 * on another thread and has not released it yet. The hint tells the two apart, and the
	 * second clears as soon as the pop finishes.
	 */
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

	/**
	 * Scheduler side of a fiber. The seam's Fiber owns the stack and registers; this holds
	 * what the loop and the counters need.
	 */
	struct FiberRecord
	{
		Fiber* fiber   = nullptr;
		Worker* worker = nullptr; // written by whoever switches to the fiber, right before the switch
		std::atomic<Counter*> wait_counter{nullptr}; // from wait() until it returns; park() and dumps read it
		FiberRecord* next_waiter = nullptr;			 // link in the counter's wait list
		const char* job_name	 = nullptr;			 // while a job runs on this worker
		char name[24]			 = {};				 // persistent, the profiler keys fibers by this pointer
		bool pool				 = false; // a pool fiber; thread records are the thread itself to the profiler
	};

	/**
	 * The pool fibers and their records. A fiber goes back to the free list when it has
	 * nothing left to run, and the list holds every fiber, so a push never fails for room.
	 */
	struct FiberPool
	{
		FiberRecord* records = nullptr; // raw storage; records are linked into lists, so they never move
		u32 count			 = 0;
		MpmcQueue<FiberRecord*> free_list{MemoryTag::Engine};

		FiberPool() noexcept = default;
		~FiberPool() noexcept;

		// Creates every fiber on entry, with its own record as the argument.
		void init(u32 fiber_count, size_t stack_size, FiberEntry entry) noexcept;

		[[nodiscard]] FiberRecord* pop_free() noexcept;
		void push_free(FiberRecord* fiber) noexcept;
		[[nodiscard]] u32 free_count() const noexcept;
	};

	/**
	 * What the fiber that just switched away needs done once it is safely off its stack.
	 * The fiber that receives control does it first thing.
	 */
	enum class Pending : u8
	{
		None,
		Park, // link it into its counter's wait list
		Free  // return it to the free list
	};

	/**
	 * Where idle workers sleep. A worker with nothing runnable sets its bit in the mask and
	 * waits on its own epoch; a producer that finds the bit set claims it and bumps that epoch.
	 * The two sides form a Dekker exchange: the sleeper announces, then checks for work, and
	 * the producer publishes work, then checks the mask. Both touch the mask with read-modify-writes,
	 * so those form one total order: either the producer sees the bit and wakes the worker, or the
	 * work went up before the bit did and the check finds it.
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

		// Sleeps the worker until a wake-up, unless busy() finds work after the annoucement.
		template <class Busy> void sleep(u32 index, Busy&& busy) noexcept;

		void wake_one(u32 index) noexcept;
		void wake_some(u32 wanted) noexcept;
		void wake_all() noexcept;
	};

	/**
	 * The wait list: fibers whose counter was unfinished when they looked, linked under the
	 * counter's address. Buckets spread the lock. The counter itself is never locked, so a
	 * completion is one atomic subtract and a waiter may destroy its counter the instant it
	 * reads zero; the last completion finds the waiters by address alone.
	 */
	struct WaitList
	{
		static constexpr u32 BUCKET_COUNT = 64;

		struct alignas(EMBER_CACHE_LINE) Bucket
		{
			std::atomic<bool> locked{false};
			FiberRecord* head = nullptr; // under the lock

			void lock() noexcept;
			void unlock() noexcept;
		};

		Bucket buckets[BUCKET_COUNT];

		[[nodiscard]] Bucket& bucket_for(const Counter* counter) noexcept;

		// Links the fiber under its counter, or returns false when the counter is already zero.
		[[nodiscard]] bool park(FiberRecord* fiber) noexcept;

		// Unlinks every fiber waiting on the counter, chained through next_waiter.
		[[nodiscard]] FiberRecord* take(const Counter* counter) noexcept;
	};

	/**
	 * The IO threads: where submit_io() tasks run, outside the scheduler. A task here may block as
	 * long as it likes and no worker notices. It completes its counter the way every job does, so
	 * a fiber parked on a read wakes the moment the bytes are in, on whichever worker is free.
	 *
	 * One queue per priority, drained High first. The semaphore holds one token per queued task:
	 * a thread sleeps on it until a submit releases one, and shutdown releases one per thread to
	 * let them out.
	 */
	struct IoThreads
	{
		static constexpr u32 MAX_THREADS	= 8; // past this the device is the limit, not the queue
		static constexpr u32 PRIORITY_COUNT = static_cast<u32>(JobPriority::Count);

		Scheduler* scheduler = nullptr;
		std::counting_semaphore<> tokens{0};
		std::atomic<bool> stopping{false};
		Vector<std::thread> threads{&memory::heap(MemoryTag::Engine)};

		MpmcQueue<Job> queues[PRIORITY_COUNT] = {
			MpmcQueue<Job>(MemoryTag::Engine),
			MpmcQueue<Job>(MemoryTag::Engine),
			MpmcQueue<Job>(MemoryTag::Engine),
		};

		void init(Scheduler& owner, u32 count, u32 queue_capacity) noexcept;

		// Joins the threads. Every submitted task must be complete.
		void shutdown() noexcept;

		[[nodiscard]] Result<void, IoSubmitError> submit(const IoTask& task, Counter& completion) noexcept;
		[[nodiscard]] u32 queued() const noexcept;

		void run(u32 index) noexcept;
		[[nodiscard]] bool take(Job& job) noexcept;
	};

	/**
	 * One worker thread. Worker 0 is the thread that initialized the job system.
	 * The others are created with the system and live until it is shut down.
	 */
	struct Worker
	{
		Scheduler* scheduler = nullptr;
		u32 index			 = 0;
		FiberRecord* current = nullptr; // fiber running on this thread
		FiberRecord* first	 = nullptr; // taken at construction, handed to the thread where it starts
		FiberRecord thread_record;		// the thread's own stack
		Pending pending			   = Pending::None;
		FiberRecord* pending_fiber = nullptr;
		std::thread thread;	   // empty for worker 0
		ProfileZone segment{}; // the fiber this thread is running, on its own track
	};

	struct Scheduler
	{
		static constexpr u32 PRIORITY_COUNT = static_cast<u32>(JobPriority::Count);
		static constexpr u32 IDLE_SPINS		= 128;

		JobSystemDef def;
		std::atomic<bool> stopping{false};

		FiberPool fibers;
		MpmcQueue<FiberRecord*> ready{MemoryTag::Engine};
		std::atomic<FiberRecord*> main_ready{nullptr}; // main, once its wait completed; only worker 0 takes it

		Worker* workers	 = nullptr;
		u32 worker_count = 0;
		Sleepers sleepers;
		IoThreads io;

		MpmcQueue<Job> jobs[PRIORITY_COUNT] = {
			MpmcQueue<Job>(MemoryTag::Engine),
			MpmcQueue<Job>(MemoryTag::Engine),
			MpmcQueue<Job>(MemoryTag::Engine),
		};

		WaitList wait_list;

		std::atomic<u32> parked{0};		  // fibers linked into the wait list
		std::atomic<u64> stalls{0};		  // waits that found no fiber
		std::atomic<bool> failing{false}; // a stalled worker is dumping and failing

		explicit Scheduler(const JobSystemDef& def) noexcept;
		~Scheduler() noexcept;

		// The one fiber with a home thread.
		[[nodiscard]] FiberRecord* main_fiber() noexcept { return &workers[0].thread_record; }

		// Counters. A counteris an atomic count; the fibers waiting on  it live in wait_list.
		void add(Counter& counter, u32 count) noexcept;
		void complete(Counter& counter) noexcept;
		void park(FiberRecord* fiber) noexcept;

		// Fibers.
		FiberRecord* pop_ready(Worker& worker) noexcept;
		FiberRecord* pop_fiber(Worker& worker) noexcept;
		void make_ready(FiberRecord* fiber) noexcept;
		void switch_to(Worker& worker, FiberRecord* from, FiberRecord* to) noexcept;
		void finish_switch(Worker& worker) noexcept;
		Worker* yield_to(FiberRecord* self, FiberRecord* next, Pending action) noexcept;

		// Workers.
		void worker_main(Worker& worker) noexcept;
		void adopt_main() noexcept;
		void release_main() noexcept;
		bool has_work(const Worker& worker) const noexcept;
		void idle(Worker& worker) noexcept;

		// Jobs.
		void kick(Span<const JobDef> jobs, Counter* counter) noexcept;
		bool pop_job(Job& job) noexcept;
		void run_job(FiberRecord* self, const Job& job) noexcept;
		void loop(FiberRecord* self) noexcept;
		void wait(Counter& counter) noexcept;
		FiberRecord* stall(FiberRecord* self, Counter& counter) noexcept;

		// Diganostics.
		void dump(const char* reason) noexcept;
		const char* fiber_label(const FiberRecord& fiber) const noexcept;
		JobStats stats() const noexcept;
	};

	/** A call boundary, so no caller keeps the thread local's address across a switch. */
	EMBER_NOINLINE Worker* current_worker() noexcept;
}
