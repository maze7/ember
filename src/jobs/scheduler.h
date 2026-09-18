#pragma once

#include <ember/jobs/job_system.h>

#include <ember/containers/mpmc_queue.h>
#include <ember/core/profile.h>
#include <jobs/fiber.h>

#include <atomic>
#include <chrono>
#include <thread>

/**
 * The scheduler behind the jobs API: fiber records, the counter and fiber pools,
 * the sleep protocol, the workers and the loop. Private to the module.
 */
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
		u32 id					 = NO_FIBER; // 1 based
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
		void run_main(JobFn main, void* data) noexcept;
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

	/** A call boundary, so no caller keeps the thread local's address accross a switch. */
	EMBER_NOINLINE Worker* current_worker() noexcept;
}
