#include <ember/jobs/job_system.h>

#include <ember/containers/mpmc_queue.h>
#include <ember/core/profile.h>
#include <ember/memory/memory.h>
#include <ember/sync/spin_mutex.h>
#include <ember/sync/thread.h>
#include <jobs/fiber.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>
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
		u32 wait_target			 = 0;
		u16 wait_generation		 = 0;
		FiberRecord* next_waiter = nullptr;
		Job handoff				 = {};		// job passed over by a fiber of the other stack class
		bool pool				 = false;	// a pool fiber; thread records are not fibers to the profiler
		char name[24]			 = {};		// persistent, the profiler keys fibers by this pointer
		const char* job_name	 = nullptr; // while a job runs on this fiber
	};

	// One slot of the counter pool, sized to a cache line so completing workers never share
	// one with a neighbour. value counts jobs in flight. The slot is freed under the lock by
	// the owner's release once the batch is complete, or by the last completion after a
	// release; freeing changes the generation, so a handle to a released batch fails to
	// resolve, and a completion or park that arrives late sees the change under the lock
	// and backs out instead of touching whoever reuses the slot.
	struct alignas(EMBER_CACHE_LINE) JobCounter
	{
		std::atomic<u32> value{0};
		std::atomic<u32> external_waiters{0}; // threads blocked in wait_blocking
		std::atomic<u16> generation{1};
		bool released = false; // under the lock
		std::atomic<bool> locked{false};
		std::atomic<FiberRecord*> waiters{nullptr};
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
		static constexpr u32 STACK_COUNT = static_cast<u32>(JobStack::Count);
		static constexpr u32 QUEUE_COUNT = static_cast<u32>(JobPriority::Count) * STACK_COUNT;
		static constexpr u32 MAX_WORKERS = 64; // one bit each in the sleeping mask
		static constexpr u32 IDLE_SPINS	 = 128;

		JobSystemDef def;
		bool running = false;
		std::atomic<bool> stopping{false};

		Vector<FiberRecord> records;
		Worker* workers		 = nullptr;
		u32 worker_count	 = 0;
		JobCounter* counters = nullptr;
		u32 counter_capacity = 0;
		MpmcQueue<u16> free_counters{MemoryTag::Engine};
		MpmcQueue<FiberRecord*> ready{MemoryTag::Engine};
		MpmcQueue<FiberRecord*> free_fibers[STACK_COUNT] = {
			MpmcQueue<FiberRecord*>(MemoryTag::Engine), MpmcQueue<FiberRecord*>(MemoryTag::Engine)};
		alignas(EMBER_CACHE_LINE) std::atomic<u64> sleeping{0}; // one bit per worker parked in idle()
		MpmcQueue<Job> jobs[QUEUE_COUNT] = {
			MpmcQueue<Job>(MemoryTag::Engine),
			MpmcQueue<Job>(MemoryTag::Engine),
			MpmcQueue<Job>(MemoryTag::Engine),
			MpmcQueue<Job>(MemoryTag::Engine),
			MpmcQueue<Job>(MemoryTag::Engine),
			MpmcQueue<Job>(MemoryTag::Engine)};

		explicit Impl(const JobSystemDef& def) noexcept;
		~Impl() noexcept;

		static u32 queue_index(JobPriority priority, JobStack stack) noexcept
		{
			return static_cast<u32>(priority) * STACK_COUNT + static_cast<u32>(stack);
		}

		static void lock(JobCounter& counter) noexcept;
		static void unlock(JobCounter& counter) noexcept;

		JobCounter* resolve(JobHandle handle) noexcept;
		JobHandle allocate_counter() noexcept;
		FiberRecord* take_waiters(JobCounter& counter, u32 value) noexcept;
		void free_locked(JobCounter& counter) noexcept;
		void release(JobHandle handle) noexcept;
		void kick(Span<const JobDef> jobs, JobHandle batch) noexcept;
		void wait(JobHandle handle, u32 target) noexcept;
		void wait_blocking(JobCounter& counter, JobHandle handle, u32 target) noexcept;
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
		FiberRecord* pop_free(JobStack stack) noexcept;
		void push_free(FiberRecord* fiber) noexcept;
		bool pop_job(JobStack own, Job& job, FiberRecord*& handoff) noexcept;
		void worker_main(Worker& worker) noexcept;
		bool has_work(const Worker& worker) const noexcept;
		void idle(Worker& worker) noexcept;
		void wake_worker(u32 index) noexcept;
		void wake_sleepers(u32 count) noexcept;
		void wake_all() noexcept;
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
	}

	JobSystem::Impl::Impl(const JobSystemDef& def) noexcept : def(def), records(&memory::heap(MemoryTag::Engine))
	{
		const u32 fiber_count = def.small_fibers + def.large_fibers;

		worker_count = def.worker_count != 0 ? def.worker_count : std::max(1u, std::thread::hardware_concurrency());
		worker_count = std::min(worker_count, MAX_WORKERS);
		EMBER_ASSERT(fiber_count >= worker_count && "every worker thread needs a fiber to run its loop");

		records.resize(fiber_count);

		free_fibers[static_cast<u32>(JobStack::Small)].init(queue_capacity(def.small_fibers));
		free_fibers[static_cast<u32>(JobStack::Large)].init(queue_capacity(def.large_fibers));
		ready.init(queue_capacity(fiber_count + worker_count));

		for (MpmcQueue<Job>& queue : jobs)
			queue.init(queue_capacity(def.queue_capacity));

		counter_capacity = def.counter_capacity < 1 ? 1 : def.counter_capacity;
		EMBER_ASSERT(counter_capacity <= std::numeric_limits<u16>::max());
		counters = static_cast<JobCounter*>(
			memory::heap(MemoryTag::Engine).allocate(counter_capacity * sizeof(JobCounter), alignof(JobCounter)));
		free_counters.init(queue_capacity(counter_capacity));

		for (u32 index = 0; index < counter_capacity; ++index)
		{
			std::construct_at(counters + index);
			const bool pushed = free_counters.try_push(static_cast<u16>(index));
			EMBER_ASSERT(pushed);
			(void)pushed;
		}

		for (u32 index = 0; index < fiber_count; ++index)
		{
			FiberRecord& record = records[index];
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

		// Threads start last, once every queue and fiber they may touch exists.
		for (u32 index = 1; index < worker_count; ++index)
			workers[index].thread = std::thread([this, index] { worker_main(workers[index]); });
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
		EMBER_ASSERT(free_count == records.size() && "fibers still parked at shutdown");
		EMBER_ASSERT(ready.size_hint() == 0);
		EMBER_ASSERT(free_counters.size_hint() == counter_capacity && "batches still owned at shutdown");
		(void)free_count;

		for (FiberRecord& record : records)
			fiber_destroy(record.fiber);

		for (u32 index = 0; index < worker_count; ++index)
			std::destroy_at(workers + index);

		memory::heap(MemoryTag::Engine).deallocate(workers, worker_count * sizeof(Worker), alignof(Worker));
		memory::heap(MemoryTag::Engine)
			.deallocate(counters, counter_capacity * sizeof(JobCounter), alignof(JobCounter));
	}

	void JobSystem::Impl::lock(JobCounter& counter) noexcept
	{
		u32 spins = 0;

		while (counter.locked.exchange(true, std::memory_order_acquire))
			detail::cpu_relax(spins++);
	}

	void JobSystem::Impl::unlock(JobCounter& counter) noexcept
	{
		counter.locked.store(false, std::memory_order_release);
	}

	JobCounter* JobSystem::Impl::resolve(JobHandle handle) noexcept
	{
		if (handle.is_null() || handle.index >= counter_capacity)
			return nullptr;

		JobCounter& counter = counters[handle.index];
		return counter.generation.load(std::memory_order_acquire) == handle.generation ? &counter : nullptr;
	}

	JobHandle JobSystem::Impl::allocate_counter() noexcept
	{
		u16 index = 0;

		if (!free_counters.try_pop(index))
		{
			EMBER_ASSERT(false && "counter pool exhausted");
			return {};
		}

		JobCounter& counter = counters[index];
		EMBER_ASSERT(counter.value.load(std::memory_order_relaxed) == 0);
		EMBER_ASSERT(!counter.released);
		EMBER_ASSERT(counter.waiters.load(std::memory_order_relaxed) == nullptr);

		return JobHandle{index, counter.generation.load(std::memory_order_relaxed)};
	}

	// Under the lock. Unlinks every waiter satisfied by value and returns them as a list.
	FiberRecord* JobSystem::Impl::take_waiters(JobCounter& counter, u32 value) noexcept
	{
		FiberRecord* wake = nullptr;
		FiberRecord* keep = nullptr;

		for (FiberRecord* waiter = counter.waiters.load(std::memory_order_relaxed); waiter != nullptr;)
		{
			FiberRecord* next  = waiter->next_waiter;
			FiberRecord*& list = waiter->wait_target >= value ? wake : keep;

			waiter->next_waiter = list;
			list				= waiter;
			waiter				= next;
		}

		counter.waiters.store(keep, std::memory_order_relaxed);
		return wake;
	}

	// Under the lock, which it releases. Bumps the generation so every handle to the old
	// batch fails to resolve, then returns the slot to the pool.
	void JobSystem::Impl::free_locked(JobCounter& counter) noexcept
	{
		EMBER_ASSERT(counter.value.load(std::memory_order_relaxed) == 0);
		EMBER_ASSERT(counter.waiters.load(std::memory_order_relaxed) == nullptr && "batch freed with fibers parked");

		counter.released = false;

		u16 generation = static_cast<u16>(counter.generation.load(std::memory_order_relaxed) + 1);
		if (generation == 0)
			generation = 1;
		counter.generation.store(generation, std::memory_order_release);

		unlock(counter);

		const bool pushed = free_counters.try_push(static_cast<u16>(&counter - counters));
		EMBER_ASSERT(pushed);
		(void)pushed;
	}

	void JobSystem::Impl::release(JobHandle handle) noexcept
	{
		JobCounter* counter = resolve(handle);
		EMBER_ASSERT(counter != nullptr && "release of a batch that is already gone");

		if (counter == nullptr)
			return;

		lock(*counter);

		if (counter->generation.load(std::memory_order_relaxed) != handle.generation)
		{
			unlock(*counter);
			EMBER_ASSERT(false && "batch released twice");
			return;
		}

		EMBER_ASSERT(!counter->released && "batch released twice");

		if (counter->value.load(std::memory_order_seq_cst) != 0)
		{
			counter->released = true;
			unlock(*counter);
			return;
		}

		// The last completion may still be on its way to this lock; it backs out when it
		// finds the generation changed, so the wake-up is ours to do.
		FiberRecord* wake = take_waiters(*counter, 0);
		free_locked(*counter);

		while (wake != nullptr)
		{
			FiberRecord* next = wake->next_waiter;
			make_ready(wake);
			wake = next;
		}
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
	// complete before anything can resume it. The link is published before the value is
	// re-read and complete() subtracts before it reads the list, both sequentially
	// consistent, so one side always sees the other: no wake-up is lost.
	void JobSystem::Impl::park(FiberRecord* fiber) noexcept
	{
		JobCounter& counter = *fiber->wait_counter;

		lock(counter);

		if (counter.generation.load(std::memory_order_relaxed) != fiber->wait_generation)
		{
			unlock(counter);
			make_ready(fiber);
			return;
		}

		fiber->next_waiter = counter.waiters.load(std::memory_order_relaxed);
		counter.waiters.store(fiber, std::memory_order_seq_cst);

		if (counter.value.load(std::memory_order_seq_cst) <= fiber->wait_target)
		{
			counter.waiters.store(fiber->next_waiter, std::memory_order_relaxed);
			unlock(counter);
			make_ready(fiber);
			return;
		}

		unlock(counter);
	}

	void JobSystem::Impl::complete(JobHandle batch) noexcept
	{
		if (batch.is_null())
			return;

		JobCounter& counter = counters[batch.index];
		const u32 value		= counter.value.fetch_sub(1, std::memory_order_seq_cst) - 1;

		if (counter.external_waiters.load(std::memory_order_seq_cst) != 0)
			counter.value.notify_all();

		if (value != 0 && counter.waiters.load(std::memory_order_seq_cst) == nullptr)
			return;

		lock(counter);

		if (counter.generation.load(std::memory_order_relaxed) != batch.generation)
		{
			unlock(counter);
			return;
		}

		FiberRecord* wake = take_waiters(counter, value);

		if (value == 0 && counter.released)
			free_locked(counter);
		else
			unlock(counter);

		while (wake != nullptr)
		{
			FiberRecord* next = wake->next_waiter;
			make_ready(wake);
			wake = next;
		}
	}

	void JobSystem::Impl::make_ready(FiberRecord* fiber) noexcept
	{
		if (fiber->pinned_worker != NO_WORKER)
		{
			Worker& worker	  = workers[fiber->pinned_worker];
			const bool pushed = worker.pinned_ready.try_push(fiber);
			EMBER_ASSERT(pushed && "pinned ready queue sized for every fiber");
			(void)pushed;

			wake_worker(worker.index);
			return;
		}

		const bool pushed = ready.try_push(fiber);
		EMBER_ASSERT(pushed && "ready queue sized for every fiber");
		(void)pushed;

		wake_sleepers(1);
	}

	FiberRecord* JobSystem::Impl::pop_ready(Worker& worker) noexcept
	{
		FiberRecord* fiber = nullptr;

		if (worker.pinned_ready.try_pop(fiber))
			return fiber;

		return ready.try_pop(fiber) ? fiber : nullptr;
	}

	FiberRecord* JobSystem::Impl::pop_free(JobStack stack) noexcept
	{
		FiberRecord* fiber = nullptr;
		return free_fibers[static_cast<u32>(stack)].try_pop(fiber) ? fiber : nullptr;
	}

	void JobSystem::Impl::push_free(FiberRecord* fiber) noexcept
	{
		const bool pushed = free_fibers[static_cast<u32>(fiber->stack)].try_push(fiber);
		EMBER_ASSERT(pushed && "free list sized for every fiber");
		(void)pushed;
	}

	// Highest priority first, own stack class before the other. A large stack fits a small
	// job, so a large fiber runs small jobs directly. A small fiber takes a large job only
	// once a free large fiber is in hand, so no job is ever popped without a stack for it.
	bool JobSystem::Impl::pop_job(JobStack own, Job& job, FiberRecord*& handoff) noexcept
	{
		handoff = nullptr;

		for (u32 priority = 0; priority < static_cast<u32>(JobPriority::Count); ++priority)
		{
			if (jobs[priority * STACK_COUNT + static_cast<u32>(own)].try_pop(job))
				return true;

			if (own == JobStack::Large)
			{
				if (jobs[priority * STACK_COUNT + static_cast<u32>(JobStack::Small)].try_pop(job))
					return true;

				continue;
			}

			MpmcQueue<Job>& queue = jobs[priority * STACK_COUNT + static_cast<u32>(JobStack::Large)];
			if (queue.size_hint() == 0)
				continue;

			FiberRecord* fiber = pop_free(JobStack::Large);
			if (fiber == nullptr)
				continue;

			if (queue.try_pop(job))
			{
				handoff = fiber;
				return true;
			}

			push_free(fiber);
		}

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
	// and an unfinished job; new jobs start only when nothing is waiting to resume. A fiber
	// that switches to another one has nothing left to do and frees itself in the process.
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
			FiberRecord* target = nullptr;

			if (pop_job(self->stack, job, target))
			{
				if (target == nullptr)
				{
					run_job(self, job);
					worker = self->worker;
					continue;
				}

				target->handoff		  = job;
				worker->pending		  = Pending::Free;
				worker->pending_fiber = self;
				switch_to(*worker, self, target);

				worker = self->worker;
				finish_switch(*worker);
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

		FiberRecord* first = pop_free(JobStack::Small);
		if (first == nullptr)
			first = pop_free(JobStack::Large);
		EMBER_ASSERT(first != nullptr && "out of fibers");

		switch_to(worker, &worker.thread_record, first);

		finish_switch(worker);
		fiber_release_thread(worker.thread_record.fiber);
		worker.thread_record.fiber = nullptr;
		t_worker				   = nullptr;
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

		// Nothing runs while the thread sleeps, so its segment and fiber close for the
		// duration and both tracks stay blank, like an idle core in a per core view.
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

	void JobSystem::Impl::wait(JobHandle handle, u32 target) noexcept
	{
		JobCounter* counter = resolve(handle);

		if (counter == nullptr || counter->value.load(std::memory_order_acquire) <= target)
			return;

		Worker* worker = current_worker();

		if (worker == nullptr)
		{
			wait_blocking(*counter, handle, target);
			return;
		}

		FiberRecord* self	  = worker->current;
		self->wait_counter	  = counter;
		self->wait_target	  = target;
		self->wait_generation = handle.generation;

		FiberRecord* next = pop_ready(*worker);

		if (next == nullptr)
			next = pop_free(JobStack::Small);

		if (next == nullptr)
			next = pop_free(JobStack::Large);

		EMBER_ASSERT(next != nullptr && "out of fibers");

		worker->pending		  = Pending::Park;
		worker->pending_fiber = self;
		switch_to(*worker, self, next);

		worker = self->worker;
		finish_switch(*worker);
	}

	// Threads outside the system have no fiber to park, so they block on the value itself.
	// The waiter count and a completion's subtract are each a read-modify-write followed by
	// a load of the other, so one side always sees the other and no wake-up is lost.
	void JobSystem::Impl::wait_blocking(JobCounter& counter, JobHandle handle, u32 target) noexcept
	{
		counter.external_waiters.fetch_add(1, std::memory_order_seq_cst);

		for (;;)
		{
			if (counter.generation.load(std::memory_order_acquire) != handle.generation)
				break;

			const u32 value = counter.value.load(std::memory_order_seq_cst);
			if (value <= target)
				break;

			counter.value.wait(value, std::memory_order_seq_cst);
		}

		counter.external_waiters.fetch_sub(1, std::memory_order_relaxed);
	}

	JobSystem::JobSystem(const JobSystemDef& def) noexcept
	{
		EMBER_ASSERT(s_system == nullptr && "one job system per process");
		m_impl	 = memory::new_object<Impl>(MemoryTag::Engine, def);
		s_system = m_impl;
	}

	JobSystem::~JobSystem() noexcept
	{
		EMBER_ASSERT(!m_impl->running);
		s_system = nullptr;
		memory::delete_object(MemoryTag::Engine, m_impl);
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

	void JobSystem::Impl::kick(Span<const JobDef> def, JobHandle batch) noexcept
	{
		if (!batch.is_null())
			counters[batch.index].value.fetch_add(static_cast<u32>(def.size()), std::memory_order_relaxed);

		for (const JobDef& def : def)
		{
			EMBER_ASSERT(def.fn != nullptr);

			const bool pushed = jobs[queue_index(def.priority, def.stack)].try_push(
				{.fn = def.fn, .data = def.data, .name = def.name, .batch = batch});
			EMBER_ASSERT(pushed && "job queue full");
			(void)pushed;
		}

		wake_sleepers(static_cast<u32>(def.size()));
	}

	JobHandle kick(Span<const JobDef> jobs) noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");

		const JobHandle handle = s_system->allocate_counter();
		if (handle.is_null())
			return handle;

		s_system->kick(jobs, handle);
		return handle;
	}

	void kick(Span<const JobDef> jobs, JobHandle batch) noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");

		EMBER_ASSERT(s_system->resolve(batch) != nullptr && "kick onto a released batch");
		s_system->kick(jobs, batch);
	}

	void kick_detached(Span<const JobDef> jobs) noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");
		s_system->kick(jobs, {});
	}

	void wait(JobHandle batch, u32 target) noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");
		s_system->wait(batch, target);
	}

	bool is_complete(JobHandle batch) noexcept
	{
		EMBER_ASSERT(s_system != nullptr && "no job system");

		const JobCounter* counter = s_system->resolve(batch);
		return counter == nullptr || counter->value.load(std::memory_order_acquire) == 0;
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
}
