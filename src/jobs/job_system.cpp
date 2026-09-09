#include <ember/jobs/job_system.h>

#include <ember/containers/mpmc_queue.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>
#include <ember/memory/memory.h>
#include <ember/sync/spin_mutex.h>
#include <ember/memory/pmr/block_allocator.h>
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
		Job handoff				 = {};		// a large job passed over by a small fiber
		bool pool				 = false;	// a pool fiber; thread records are not fibers to the profiler
		char name[24]			 = {};		// persistent, the profiler keys fibers by this pointer
		const char* job_name	 = nullptr; // while a job runs on this fiber
		std::atomic<JobCounter*> stalled_on{nullptr}; // spinning in a wait that found no fiber; read by dumps
	};

	// One slot of the counter pool, sized to a cache line so completing workers never share
	// one with a neighbour. One word holds the generation and the jobs still running, so a
	// handle resolves and a completion subtracts on the same atomic, and a thread blocked on
	// the word wakes when either half changes. The last completion takes the waiters under
	// the lock and sets finalized, the owner's release sets released, and whichever of the
	// two comes second frees the slot, which bumps the generation.
	struct alignas(EMBER_CACHE_LINE) JobCounter
	{
		std::atomic<u64> state{0};
		std::atomic<bool> locked{false};
		bool released		 = false;	// under the lock
		bool finalized		 = false;	// under the lock
		FiberRecord* waiters = nullptr; // under the lock
	};

	static_assert(sizeof(JobCounter) == EMBER_CACHE_LINE);

	// What the fiber that just switched away needs done once it is safely off its stack.
	// The fiber that receives control does it first thing.
	enum class Pending : u8
	{
		None,
		Park, // link it into its counter's wait list
		Free  // return it to the free list
	};

	// One worker thread. Worker 0 is the thread that calls run(); the others are created
	// with the system and live until it is destroyed. A worker with nothing runnable spins
	// briefly, announces itself in the sleeping mask and waits on its epoch, which every
	// wake-up bumps.
	struct Worker
	{
		JobSystem::Impl* system = nullptr;
		u32 index				= 0;
		FiberRecord* current	= nullptr; // fiber running on this thread
		FiberRecord* first		= nullptr; // taken at construction, handed to the thread when it starts
		FiberRecord thread_record;		   // the thread's own stack: main inside run(), the exit path elsewhere
		Pending pending			   = Pending::None;
		FiberRecord* pending_fiber = nullptr;
		MpmcQueue<FiberRecord*> pinned_ready{MemoryTag::Engine}; // fibers that must resume on this worker
		std::thread thread;										 // empty for worker 0
		ProfileZone segment{};									 // the fiber this thread is running, on its own track
		alignas(EMBER_CACHE_LINE) std::atomic<u32> epoch{0};
	};

	struct JobSystem::Impl
	{
		static constexpr u32 STACK_COUNT	= static_cast<u32>(JobStack::Count);
		static constexpr u32 PRIORITY_COUNT = static_cast<u32>(JobPriority::Count);
		static constexpr u32 MAX_WORKERS	= 64; // one bit each in the sleeping mask
		static constexpr u32 IDLE_SPINS		= 128;

		JobSystemDef def;
		bool running = false;
		std::atomic<bool> stopping{false};

		FiberRecord* records = nullptr; // raw storage: a record holds an atomic, so it never moves
		u32 fiber_count		 = 0;
		Worker* workers		 = nullptr;
		u32 worker_count	 = 0;
		JobCounter* counters = nullptr;
		u32 counter_capacity = 0;
		MpmcQueue<u32> free_counters{MemoryTag::Engine};
		MpmcQueue<FiberRecord*> ready{MemoryTag::Engine};
		MpmcQueue<FiberRecord*> free_fibers[STACK_COUNT] = {
			MpmcQueue<FiberRecord*>(MemoryTag::Engine), MpmcQueue<FiberRecord*>(MemoryTag::Engine)};
		alignas(EMBER_CACHE_LINE) std::atomic<u64> sleeping{0}; // one bit per worker parked in idle()
		std::atomic<u32> parked{0};								// fibers linked into a wait list
		std::atomic<u64> stalls{0};								// waits that found no fiber
		std::atomic<bool> failing{false};						// a stalled worker is dumping and failing
		MpmcQueue<Job> jobs[PRIORITY_COUNT] = {
			MpmcQueue<Job>(MemoryTag::Engine), MpmcQueue<Job>(MemoryTag::Engine), MpmcQueue<Job>(MemoryTag::Engine)};

		explicit Impl(const JobSystemDef& def) noexcept;
		~Impl() noexcept;

		static void lock(JobCounter& counter) noexcept;
		static void unlock(JobCounter& counter) noexcept;

		JobCounter* resolve(JobHandle handle) noexcept;
		JobHandle allocate_counter(u32 count) noexcept;
		void free_locked(JobCounter& counter) noexcept;
		void release(JobHandle handle) noexcept;
		void kick(Span<const JobDef> jobs, JobHandle batch) noexcept;
		void wait(JobHandle handle) noexcept;
		FiberRecord* stall(FiberRecord* self, JobCounter& counter, JobHandle handle) noexcept;
		FiberRecord* wait_for_large_fiber() noexcept;
		void spin_or_fail(u32& spin, std::chrono::steady_clock::time_point since, const char* reason) noexcept;
		void loop(FiberRecord* self) noexcept;
		void run_job(FiberRecord* self, const Job& job) noexcept;
		static void segment_begin(Worker& worker, const FiberRecord* fiber) noexcept;
		static void segment_end(Worker& worker) noexcept;
		static void resegment(FiberRecord* self) noexcept;
		void switch_to(Worker& worker, FiberRecord* from, FiberRecord* to) noexcept;
		void finish_switch(Worker& worker) noexcept;
		void park(FiberRecord* fiber) noexcept;
		void complete(JobHandle batch) noexcept;
		void make_ready(FiberRecord* fiber) noexcept;
		FiberRecord* pop_ready(Worker& worker) noexcept;
		FiberRecord* pop_fiber(Worker& worker) noexcept;
		FiberRecord* pop_free(JobStack stack) noexcept;
		void push_free(FiberRecord* fiber) noexcept;
		bool pop_job(Job& job) noexcept;
		void worker_main(Worker& worker) noexcept;
		bool has_work(const Worker& worker) const noexcept;
		void idle(Worker& worker) noexcept;
		void wake_worker(u32 index) noexcept;
		void wake_sleepers(u32 count) noexcept;
		void wake_all() noexcept;
		void dump(const char* reason) noexcept;
		const char* fiber_label(const FiberRecord& fiber) const noexcept;
		JobStats stats() const noexcept;
	};

	namespace
	{
		// The only thread local in the system. Nothing caches its result across a switch:
		// after a wait a fiber reads its worker from its own record instead.
		thread_local Worker* t_worker = nullptr;

		JobSystem::Impl* s_system = nullptr;

		EMBER_NOINLINE Worker* current_worker() noexcept { return t_worker; }

		void fiber_main(void* arg)
		{
			auto* self = static_cast<FiberRecord*>(arg);
			self->worker->system->loop(self);
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
	}

	JobSystem::Impl::Impl(const JobSystemDef& def) noexcept : def(def)
	{
		fiber_count = def.small_fibers + def.large_fibers;

		const u32 hardware = std::max(1u, std::thread::hardware_concurrency());

		worker_count = def.worker_count != 0 ? def.worker_count : hardware - std::min(def.reserved_threads, hardware - 1);
		worker_count = std::min(worker_count, MAX_WORKERS);
		EMBER_ASSERT(fiber_count >= worker_count && "every worker thread needs a fiber to run its loop");

		records = static_cast<FiberRecord*>(
			memory::heap(MemoryTag::Engine).allocate(fiber_count * sizeof(FiberRecord), alignof(FiberRecord)));

		free_fibers[static_cast<u32>(JobStack::Small)].init(queue_capacity(def.small_fibers));
		free_fibers[static_cast<u32>(JobStack::Large)].init(queue_capacity(def.large_fibers));
		ready.init(queue_capacity(fiber_count + worker_count));

		for (MpmcQueue<Job>& queue : jobs)
			queue.init(queue_capacity(def.queue_capacity));

		counter_capacity = def.counter_capacity < 1 ? 1 : def.counter_capacity;
		counters		 = static_cast<JobCounter*>(
			memory::heap(MemoryTag::Engine).allocate(counter_capacity * sizeof(JobCounter), alignof(JobCounter)));
		free_counters.init(queue_capacity(counter_capacity));

		for (u32 index = 0; index < counter_capacity; ++index)
		{
			JobCounter& counter = *std::construct_at(counters + index);
			counter.state.store(pack_state(1, 0), std::memory_order_relaxed);
			push_held(free_counters, index);
		}

		for (u32 index = 0; index < fiber_count; ++index)
		{
			FiberRecord& record = *std::construct_at(records + index);
			record.stack		= index < def.small_fibers ? JobStack::Small : JobStack::Large;
			record.pool			= true;
			std::snprintf(record.name, sizeof(record.name), "fiber %u", index);

			const size_t stack_size = record.stack == JobStack::Small ? def.small_stack : def.large_stack;
			record.fiber			= fiber_create({.stack_size = stack_size, .entry = fiber_main, .arg = &record});
			EMBER_ASSERT(record.fiber != nullptr);

			push_free(&record);
		}

		workers = static_cast<Worker*>(
			memory::heap(MemoryTag::Engine).allocate(worker_count * sizeof(Worker), alignof(Worker)));

		for (u32 index = 0; index < worker_count; ++index)
		{
			Worker& worker = *std::construct_at(workers + index);

			worker.system					   = this;
			worker.index					   = index;
			worker.thread_record.stack		   = JobStack::Large;
			worker.thread_record.pinned_worker = index;
			worker.pinned_ready.init(queue_capacity(fiber_count + worker_count));
		}

		// Threads start last, once every queue and fiber they may touch exists. Each thread's
		// first fiber is taken here, ahead of a main that may drain the pool before the
		// thread gets to run.
		for (u32 index = 1; index < worker_count; ++index)
		{
			Worker& worker = workers[index];

			worker.first = pop_free(JobStack::Small);
			if (worker.first == nullptr)
				worker.first = pop_free(JobStack::Large);
			EMBER_ASSERT(worker.first != nullptr && "out of fibers");

			worker.thread = std::thread([this, index] { worker_main(workers[index]); });
		}
	}

	JobSystem::Impl::~Impl() noexcept
	{
		stopping.store(true, std::memory_order_release);
		wake_all();

		for (u32 index = 1; index < worker_count; ++index)
			if (workers[index].thread.joinable())
				workers[index].thread.join();

		const size_t free_count = free_fibers[static_cast<u32>(JobStack::Small)].size_hint() +
								  free_fibers[static_cast<u32>(JobStack::Large)].size_hint();
		EMBER_ASSERT(free_count == fiber_count && "fibers still parked at shutdown");
		EMBER_ASSERT(ready.size_hint() == 0);
		EMBER_ASSERT(free_counters.size_hint() == counter_capacity && "batches still owned at shutdown");
		(void)free_count;

		for (const MpmcQueue<Job>& queue : jobs)
			EMBER_ASSERT(queue.size_hint() == 0 && "jobs still queued at shutdown");

		for (u32 index = 0; index < fiber_count; ++index)
		{
			fiber_destroy(records[index].fiber);
			std::destroy_at(records + index);
		}

		for (u32 index = 0; index < worker_count; ++index)
			std::destroy_at(workers + index);

		for (u32 index = 0; index < counter_capacity; ++index)
			std::destroy_at(counters + index);

		memory::heap(MemoryTag::Engine).deallocate(records, fiber_count * sizeof(FiberRecord), alignof(FiberRecord));
		memory::heap(MemoryTag::Engine).deallocate(workers, worker_count * sizeof(Worker), alignof(Worker));
		memory::heap(MemoryTag::Engine)
			.deallocate(counters, counter_capacity * sizeof(JobCounter), alignof(JobCounter));
	}

	// Test before the exchange, so a spinner reads the line shared instead of bouncing it.
	void JobSystem::Impl::lock(JobCounter& counter) noexcept
	{
		for (u32 spins = 0;; detail::cpu_relax(spins++))
		{
			if (counter.locked.load(std::memory_order_relaxed))
				continue;

			if (!counter.locked.exchange(true, std::memory_order_acquire))
				return;
		}
	}

	void JobSystem::Impl::unlock(JobCounter& counter) noexcept
	{
		counter.locked.store(false, std::memory_order_release);
	}

	JobCounter* JobSystem::Impl::resolve(JobHandle handle) noexcept
	{
		if (handle.is_null() || handle.index >= counter_capacity)
			return nullptr;

		return counters + handle.index;
	}

	// The slot is nobody's until the handle is returned, so plain stores suffice; the count
	// is in place before any job can subtract from it.
	JobHandle JobSystem::Impl::allocate_counter(u32 count) noexcept
	{
		u32 index = 0;

		if (!pop_any(free_counters, index))
			fail("counter pool exhausted, raise JobSystemDef::counter_capacity or release batches sooner");

		JobCounter& counter	 = counters[index];
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
	void JobSystem::Impl::free_locked(JobCounter& counter) noexcept
	{
		EMBER_ASSERT(counter.released && counter.finalized && counter.waiters == nullptr);

		u32 generation = generation_of(counter.state.load(std::memory_order_relaxed)) + 1;
		if (generation == 0)
			generation = 1;

		counter.state.store(pack_state(generation, 0), std::memory_order_release);
		unlock(counter);

		push_held(free_counters, static_cast<u32>(&counter - counters));
	}

	void JobSystem::Impl::release(JobHandle handle) noexcept
	{
		JobCounter* counter = resolve(handle);
		EMBER_ASSERT(counter != nullptr && "release of a null or foreign batch");

		if (counter == nullptr)
			return;

		lock(*counter);

		if (generation_of(counter->state.load(std::memory_order_relaxed)) != handle.generation || counter->released)
		{
			unlock(*counter);
			EMBER_ASSERT(false && "batch released twice");
			return;
		}

		counter->released = true;

		if (counter->finalized)
			free_locked(*counter);
		else
			unlock(*counter);
	}

	// A segment is a zone on the thread's own track covering the time the thread spends
	// running one fiber, named after the job on it. Fiber tracks show a job whole; thread
	// tracks show where each piece of it ran, the view of a per core job profiler.
	void JobSystem::Impl::segment_begin(Worker& worker, const FiberRecord* fiber) noexcept
	{
#if EMBER_USE_TRACY
		EMBER_PROFILE_ZONE_BEGIN(worker.segment, "fiber");

		const char* text = fiber->job_name != nullptr ? fiber->job_name : fiber->name;
		EMBER_PROFILE_ZONE_RENAME(worker.segment, text, std::strlen(text));
#else
		(void)worker;
		(void)fiber;
#endif
	}

	void JobSystem::Impl::segment_end(Worker& worker) noexcept
	{
		EMBER_PROFILE_ZONE_END(worker.segment);
		(void)worker;
	}

	// Returns when from is resumed, on whatever thread resumes it.
	void JobSystem::Impl::switch_to(Worker& worker, FiberRecord* from, FiberRecord* to) noexcept
	{
		EMBER_ASSERT(from != to);
		to->worker	   = &worker;
		worker.current = to;

		if (from->pool)
		{
			EMBER_PROFILE_FIBER_LEAVE();
			segment_end(worker);
		}

		fiber_switch(from->fiber, to->fiber);

		if (from->pool)
		{
			segment_begin(*from->worker, from);
			EMBER_PROFILE_FIBER_ENTER(from->name);
		}
	}

	void JobSystem::Impl::finish_switch(Worker& worker) noexcept
	{
		FiberRecord* fiber	 = worker.pending_fiber;
		const Pending action = worker.pending;

		worker.pending		 = Pending::None;
		worker.pending_fiber = nullptr;

		if (action == Pending::Park)
			park(fiber);
		else if (action == Pending::Free)
			push_free(fiber);
	}

	// Runs on the fiber that took over from the parking one, so the parked context is
	// complete before anything can resume it. Under the lock the last completion has either
	// zeroed the word already, which this check sees, or has not taken the list yet, in which
	// case it finds the link.
	void JobSystem::Impl::park(FiberRecord* fiber) noexcept
	{
		JobCounter& counter = *fiber->wait_counter;

		lock(counter);

		const u64 state = counter.state.load(std::memory_order_acquire);

		if (generation_of(state) != fiber->wait_generation || remaining_of(state) == 0)
		{
			unlock(counter);
			make_ready(fiber);
			return;
		}

		fiber->next_waiter = counter.waiters;
		counter.waiters	   = fiber;
		parked.fetch_add(1, std::memory_order_relaxed);

		unlock(counter);
	}

	void JobSystem::Impl::complete(JobHandle batch) noexcept
	{
		if (batch.is_null())
			return;

		JobCounter& counter = counters[batch.index];
		const u64 previous	= counter.state.fetch_sub(1, std::memory_order_acq_rel);

		EMBER_ASSERT(generation_of(previous) == batch.generation && remaining_of(previous) != 0);

		if (remaining_of(previous) != 1)
			return;

		// The last job. Threads blocked on the word wake here; parked fibers are taken under
		// the lock, and the owner's release may already be waiting for finalized.
		counter.state.notify_all();

		lock(counter);

		FiberRecord* wake = counter.waiters;
		counter.waiters	  = nullptr;
		counter.finalized = true;

		if (counter.released)
			free_locked(counter);
		else
			unlock(counter);

		while (wake != nullptr)
		{
			FiberRecord* next = wake->next_waiter;
			parked.fetch_sub(1, std::memory_order_relaxed);
			make_ready(wake);
			wake = next;
		}
	}

	void JobSystem::Impl::make_ready(FiberRecord* fiber) noexcept
	{
		if (fiber->pinned_worker != NO_WORKER)
		{
			Worker& worker = workers[fiber->pinned_worker];
			push_held(worker.pinned_ready, fiber);
			wake_worker(worker.index);
			return;
		}

		push_held(ready, fiber);
		wake_sleepers(1);
	}

	FiberRecord* JobSystem::Impl::pop_ready(Worker& worker) noexcept
	{
		FiberRecord* fiber = nullptr;

		if (worker.pinned_ready.try_pop(fiber))
			return fiber;

		return ready.try_pop(fiber) ? fiber : nullptr;
	}

	// Anything this worker may continue on: a fiber whose wait completed, else a fresh one.
	FiberRecord* JobSystem::Impl::pop_fiber(Worker& worker) noexcept
	{
		FiberRecord* next = pop_ready(worker);

		if (next == nullptr)
			next = pop_free(JobStack::Small);

		if (next == nullptr)
			next = pop_free(JobStack::Large);

		return next;
	}

	FiberRecord* JobSystem::Impl::pop_free(JobStack stack) noexcept
	{
		FiberRecord* fiber = nullptr;
		return free_fibers[static_cast<u32>(stack)].try_pop(fiber) ? fiber : nullptr;
	}

	void JobSystem::Impl::push_free(FiberRecord* fiber) noexcept
	{
		push_held(free_fibers[static_cast<u32>(fiber->stack)], fiber);
	}

	bool JobSystem::Impl::pop_job(Job& job) noexcept
	{
		for (MpmcQueue<Job>& queue : jobs)
			if (queue.try_pop(job))
				return true;

		return false;
	}

	// Segments live on the thread track, so the fiber steps out while its segment is split.
	void JobSystem::Impl::resegment(FiberRecord* self) noexcept
	{
		EMBER_PROFILE_FIBER_LEAVE();
		segment_end(*self->worker);
		segment_begin(*self->worker, self);
		EMBER_PROFILE_FIBER_ENTER(self->name);
	}

	void JobSystem::Impl::run_job(FiberRecord* self, const Job& job) noexcept
	{
		self->job_name = job.name != nullptr ? job.name : "job";
		resegment(self);

		{
			EMBER_PROFILE_SCOPE("job");
			EMBER_PROFILE_ZONE_NAME(self->job_name, std::strlen(self->job_name));
			job.fn(job.data);
		}

		self->job_name = nullptr;
		resegment(self);

		complete(job.batch);
	}

	// Every pool fiber runs this forever. Ready fibers come first because they hold a stack
	// and an unfinished job; new jobs start only when nothing is waiting to resume. A large
	// fiber runs any job. A small fiber hands a large job to a large fiber, which runs it
	// first thing. A fiber that switches to another one has nothing left to do and frees
	// itself in the process.
	void JobSystem::Impl::loop(FiberRecord* self) noexcept
	{
		segment_begin(*self->worker, self);
		EMBER_PROFILE_FIBER_ENTER(self->name);

		Worker* worker = self->worker;
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
				worker->pending		  = Pending::Free;
				worker->pending_fiber = self;
				switch_to(*worker, self, next);

				worker = self->worker;
				finish_switch(*worker);
				continue;
			}

			Job job;

			if (pop_job(job))
			{
				if (job.stack == JobStack::Large && self->stack == JobStack::Small)
				{
					FiberRecord* target	  = wait_for_large_fiber();
					target->handoff		  = job;
					worker->pending		  = Pending::Free;
					worker->pending_fiber = self;
					switch_to(*worker, self, target);

					worker = self->worker;
					finish_switch(*worker);
					continue;
				}

				run_job(self, job);
				worker = self->worker;
				continue;
			}

			if (stopping.load(std::memory_order_acquire))
			{
				worker->pending		  = Pending::Free;
				worker->pending_fiber = self;
				switch_to(*worker, self, &worker->thread_record);
				EMBER_UNREACHABLE_ASSERT();
			}

			idle(*worker);
		}
	}

	// Worker threads live here. The thread's own stack becomes its thread record, used only
	// to leave the loop at shutdown; everything else runs on pool fibers.
	void JobSystem::Impl::worker_main(Worker& worker) noexcept
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

	bool JobSystem::Impl::has_work(const Worker& worker) const noexcept
	{
		if (worker.pinned_ready.size_hint() != 0 || ready.size_hint() != 0)
			return true;

		for (const MpmcQueue<Job>& queue : jobs)
			if (queue.size_hint() != 0)
				return true;

		return false;
	}

	// Spin a little for work that is about to arrive, then sleep. The announcement and the
	// producers' sleeper checks are all read-modify-writes on the sleeping mask, so they
	// form one total order: a producer either sees the bit and wakes this worker, or its
	// work was published before the bit went up and the check below finds it. The epoch is
	// read before the announcement, so a wake-up landing between the announcement and the
	// wait shows up as a changed value and the wait returns at once.
	void JobSystem::Impl::idle(Worker& worker) noexcept
	{
		for (u32 spin = 0; spin < IDLE_SPINS; ++spin)
		{
			if (has_work(worker) || stopping.load(std::memory_order_relaxed))
				return;

			detail::cpu_relax(spin);
		}

		EMBER_PROFILE_FIBER_LEAVE();
		segment_end(worker);

		const u32 observed = worker.epoch.load(std::memory_order_acquire);
		const u64 bit	   = u64{1} << worker.index;

		sleeping.fetch_or(bit, std::memory_order_acq_rel);

		if (!has_work(worker) && !stopping.load(std::memory_order_acquire))
			worker.epoch.wait(observed, std::memory_order_acquire);

		sleeping.fetch_and(~bit, std::memory_order_relaxed);

		segment_begin(worker, worker.current);
		EMBER_PROFILE_FIBER_ENTER(worker.current->name);
	}

	// Producers observe the mask with a read-modify-write rather than a load: that orders it
	// after the work they just published, against the sleeper's own read-modify-write.
	void JobSystem::Impl::wake_worker(u32 index) noexcept
	{
		const u64 bit = u64{1} << index;

		if ((sleeping.fetch_or(0, std::memory_order_acq_rel) & bit) == 0)
			return;

		if ((sleeping.fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0)
			return;

		workers[index].epoch.fetch_add(1, std::memory_order_release);
		workers[index].epoch.notify_one();
	}

	// Claims sleepers one bit at a time so several producers do not all wake the same one.
	void JobSystem::Impl::wake_sleepers(u32 count) noexcept
	{
		u64 mask = sleeping.fetch_or(0, std::memory_order_acq_rel);

		while (mask != 0 && count != 0)
		{
			const u32 index	 = static_cast<u32>(std::countr_zero(mask));
			const u64 bit	 = u64{1} << index;
			mask			&= ~bit;

			if ((sleeping.fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0)
				continue;

			workers[index].epoch.fetch_add(1, std::memory_order_release);
			workers[index].epoch.notify_one();
			--count;
		}
	}

	void JobSystem::Impl::wake_all() noexcept
	{
		for (u32 index = 0; index < worker_count; ++index)
		{
			workers[index].epoch.fetch_add(1, std::memory_order_release);
			workers[index].epoch.notify_all();
		}
	}

	void JobSystem::Impl::wait(JobHandle handle) noexcept
	{
		JobCounter* counter = resolve(handle);
		if (counter == nullptr)
			return;

		u64 state = counter->state.load(std::memory_order_acquire);
		if (generation_of(state) != handle.generation || remaining_of(state) == 0)
			return;

		Worker* worker = current_worker();

		if (worker == nullptr)
		{
			// Threads outside the system have no fiber to park, so they block on the word. A
			// reuse of the slot changes the same word, so a stale sleeper wakes and leaves.
			do
			{
				counter->state.wait(state, std::memory_order_acquire);
				state = counter->state.load(std::memory_order_acquire);
			} while (generation_of(state) == handle.generation && remaining_of(state) != 0);

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

		worker->pending		  = Pending::Park;
		worker->pending_fiber = self;
		switch_to(*worker, self, next);

		worker = self->worker;
		finish_switch(*worker);
	}

	// Every fiber is parked or busy on another worker. The wait spins for one to come back or
	// for its own batch to finish, and after stall_report_ms of neither the pool is too small
	// for the job graph: the dump names the batches holding it.
	FiberRecord* JobSystem::Impl::stall(FiberRecord* self, JobCounter& counter, JobHandle handle) noexcept
	{
		EMBER_PROFILE_SCOPE_C("stalled wait", PROFILE_COLOR_WAIT);
		stalls.fetch_add(1, std::memory_order_relaxed);
		self->stalled_on.store(&counter, std::memory_order_relaxed);

		Worker& worker	   = *self->worker;
		const auto since   = std::chrono::steady_clock::now();
		FiberRecord* next  = nullptr;

		for (u32 spin = 0;;)
		{
			const u64 state = counter.state.load(std::memory_order_acquire);
			if (generation_of(state) != handle.generation || remaining_of(state) == 0)
				break;

			next = pop_fiber(worker);
			if (next != nullptr)
				break;

			spin_or_fail(spin, since, "a wait found no fiber and nothing finished");
		}

		self->stalled_on.store(nullptr, std::memory_order_relaxed);
		return next;
	}

	// A small fiber holding a large job. Large fibers come back as large jobs finish; none for
	// stall_report_ms means the large pool is too small for the job graph.
	FiberRecord* JobSystem::Impl::wait_for_large_fiber() noexcept
	{
		const auto since = std::chrono::steady_clock::now();

		for (u32 spin = 0;;)
		{
			if (FiberRecord* fiber = pop_free(JobStack::Large))
				return fiber;

			spin_or_fail(spin, since, "a large job found no large fiber");
		}
	}

	// One turn of a spin that may not end: the clock is read every 1024 turns, and a spin
	// older than stall_report_ms logs the state and fails. The first worker there reports;
	// the others keep spinning for the moment the failure takes.
	void JobSystem::Impl::spin_or_fail(
		u32& spin, std::chrono::steady_clock::time_point since, const char* reason) noexcept
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

	void JobSystem::Impl::dump(const char* reason) noexcept
	{
		char line[512];

		const JobStats snapshot = stats();

		std::snprintf(
			line,
			sizeof(line),
			"%s: %u small and %u large fibers free, %u parked, %u ready, %u jobs queued, %u batches live",
			reason,
			snapshot.free_small_fibers,
			snapshot.free_large_fibers,
			snapshot.parked_fibers,
			snapshot.ready_fibers,
			snapshot.queued_jobs,
			snapshot.live_batches);
		EMBER_ERROR("(ember::jobs) {}", line);

		for (u32 index = 0; index < counter_capacity; ++index)
		{
			JobCounter& counter = counters[index];
			lock(counter);

			const u32 remaining = remaining_of(counter.state.load(std::memory_order_relaxed));
			FiberRecord* waiter = counter.waiters;

			if (remaining == 0 && waiter == nullptr)
			{
				unlock(counter);
				continue;
			}

			int used = std::snprintf(line, sizeof(line), "  batch %u: %u jobs left", index, remaining);

			for (; waiter != nullptr && used < static_cast<int>(sizeof(line)); waiter = waiter->next_waiter)
				used += std::snprintf(line + used, sizeof(line) - used, ", %s parked", fiber_label(*waiter));

			unlock(counter);

			// Stalled waits sit on no list; their fibers name the counter. Read racily, this
			// is a diagnostic.
			const auto add_stalled = [&](const FiberRecord& fiber)
			{
				if (fiber.stalled_on.load(std::memory_order_relaxed) != &counter ||
					used >= static_cast<int>(sizeof(line)))
					return;

				used += std::snprintf(line + used, sizeof(line) - used, ", %s stalled", fiber_label(fiber));
			};

			for (u32 fiber = 0; fiber < fiber_count; ++fiber)
				add_stalled(records[fiber]);
			for (u32 worker = 0; worker < worker_count; ++worker)
				add_stalled(workers[worker].thread_record);

			EMBER_ERROR("(ember::jobs) {}", line);
		}
	}

	const char* JobSystem::Impl::fiber_label(const FiberRecord& fiber) const noexcept
	{
		if (&fiber == &workers[0].thread_record)
			return "main";

		return fiber.job_name != nullptr ? fiber.job_name : fiber.name;
	}

	JobStats JobSystem::Impl::stats() const noexcept
	{
		size_t queued = 0;
		for (const MpmcQueue<Job>& queue : jobs)
			queued += queue.size_hint();

		size_t ready_count = ready.size_hint();
		for (u32 index = 0; index < worker_count; ++index)
			ready_count += workers[index].pinned_ready.size_hint();

		return {
			.free_small_fibers = static_cast<u32>(free_fibers[static_cast<u32>(JobStack::Small)].size_hint()),
			.free_large_fibers = static_cast<u32>(free_fibers[static_cast<u32>(JobStack::Large)].size_hint()),
			.parked_fibers	   = parked.load(std::memory_order_relaxed),
			.ready_fibers	   = static_cast<u32>(ready_count),
			.queued_jobs	   = static_cast<u32>(queued),
			.live_batches	   = static_cast<u32>(counter_capacity - free_counters.size_hint()),
			.stalls			   = stalls.load(std::memory_order_relaxed),
		};
	}

	JobSystem::JobSystem(const JobSystemDef& def) noexcept
	{
		EMBER_ASSERT(s_system == nullptr && "one job system per process");
		m_impl	 = memory::new_object<Impl>(MemoryTag::Engine, def);
		s_system = m_impl;
	}

	// Workers drain their queues inside the destructor and a draining job may kick, so the
	// global goes last.
	JobSystem::~JobSystem() noexcept
	{
		EMBER_ASSERT(!m_impl->running);
		memory::delete_object(MemoryTag::Engine, m_impl);
		s_system = nullptr;
	}

	void JobSystem::run(JobFn main, void* data) noexcept
	{
		Impl& impl = *m_impl;
		EMBER_ASSERT(!impl.running && "run is not re-entrant");
		impl.running = true;

		Worker& worker				= impl.workers[0];
		worker.thread_record.fiber	= fiber_adopt_thread();
		worker.thread_record.worker = &worker;
		worker.current				= &worker.thread_record;
		t_worker					= &worker;

		main(data);

		EMBER_ASSERT(worker.current == &worker.thread_record);

		t_worker = nullptr;
		fiber_release_thread(worker.thread_record.fiber);
		worker.thread_record.fiber = nullptr;
		impl.running			   = false;
	}

	void JobSystem::Impl::kick(Span<const JobDef> defs, JobHandle batch) noexcept
	{
		for (const JobDef& job : defs)
		{
			EMBER_ASSERT(job.fn != nullptr);
			EMBER_ASSERT((job.stack != JobStack::Large || def.large_fibers != 0) && "no large fibers configured");

			const bool pushed = push_or_full(
				jobs[static_cast<u32>(job.priority)],
				Job{.fn = job.fn, .data = job.data, .name = job.name, .batch = batch, .stack = job.stack});

			if (!pushed)
				fail("job queue full, raise JobSystemDef::queue_capacity");
		}

		wake_sleepers(static_cast<u32>(defs.size()));
	}

	JobHandle kick(Span<const JobDef> jobs) noexcept
	{
		if (jobs.empty())
			return {};

		EMBER_ASSERT(s_system != nullptr && "no job system");

		const JobHandle handle = s_system->allocate_counter(static_cast<u32>(jobs.size()));
		s_system->kick(jobs, handle);
		return handle;
	}

	void kick_detached(Span<const JobDef> jobs) noexcept
	{
		if (jobs.empty())
			return;

		EMBER_ASSERT(s_system != nullptr && "no job system");
		s_system->kick(jobs, {});
	}

	void wait(JobHandle batch) noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");
		s_system->wait(batch);
	}

	bool is_complete(JobHandle batch) noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");

		const JobCounter* counter = s_system->resolve(batch);
		if (counter == nullptr)
			return true;

		const u64 state = counter->state.load(std::memory_order_acquire);
		return generation_of(state) != batch.generation || remaining_of(state) == 0;
	}

	void release(JobHandle batch) noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");
		s_system->release(batch);
	}

	u32 worker_count() noexcept { return s_system != nullptr ? s_system->worker_count : 0; }

	u32 worker_index() noexcept
	{
		const Worker* worker = current_worker();
		return worker != nullptr ? worker->index : NO_WORKER;
	}

	JobStats stats() noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");
		return s_system->stats();
	}

	void dump_state() noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");
		s_system->dump("job system state");
	}

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
				  .fn		= run_range,
				  .data		= &ranges[i],
				  .name		= def.name,
				  .priority = def.priority,
				  .stack	= def.stack,
			  };

			begin = end;
		}

		const JobHandle batch = kick(Span<const JobDef>(defs, jobs));
		wait(batch);
		release(batch);
	}
}
