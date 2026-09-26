#include <jobs/scheduler.h>

#include <ember/core/logger.h>
#include <ember/memory/memory.h>
#include <ember/sync/spin_mutex.h>
#include <ember/sync/thread.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace ember::jobs
{
	namespace
	{
		// The only thread local in the system. Nothing caches its result across a switch:
		// after a wait a fiber reads its worker form its own record instead.
		thread_local Worker* t_worker = nullptr;

		void fiber_main(void* arg)
		{
			auto* self = static_cast<FiberRecord*>(arg);
			self->worker->scheduler->loop(self);
		}

		[[nodiscard]] size_t queue_capacity(u32 count) noexcept { return std::bit_ceil(count < 2 ? 2u : count); }

		// Running out of a pool is a configuration error, so it ends the process the way an
		// allocation failure does, in every build.
		[[noreturn]] void fail(const char* what) noexcept
		{
			EMBER_ERROR("(ember::jobs) {}", what);
			EMBER_DEBUG_BREAK();
			std::abort();
		}

		// Pools never fill: an object is pushed only by the holder that took it out.
		template <class T> void push_held(MpmcQueue<T>& queue, const T& value) noexcept
		{
			const bool pushed = push_or_full(queue, value);
			EMBER_ASSERT(pushed && "pool queue sized for every object");
			(void)pushed;
		}

		// A segment is a zone on the thread's own track for the time the thread spends running
		// one fiber, named after the job on it: fiber tracks show a job whole, thread tracks
		// show where each piece of it ran. The profiler files every event under the fiber it has
		// entered, so the segment opens and closes in thread context: begin it before entering
		// the fiber, leave the fiber before ending it. Thread records are the thread itself to the
		// profiler and get neither.
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

	EMBER_NOINLINE Worker* current_worker() noexcept { return t_worker; }

	// Test before the exchange, so a spinner reads the line shared instead of bouncing it.
	void WaitList::Bucket::lock() noexcept
	{
		for (u32 spins = 0;; detail::cpu_relax(spins++))
		{
			if (locked.load(std::memory_order_relaxed))
				continue;

			if (!locked.exchange(true, std::memory_order_acquire))
				return;
		}
	}

	void WaitList::Bucket::unlock() noexcept { locked.store(false, std::memory_order_release); }

	// Counters are cache line aligned, so the line index is the natural key.
	WaitList::Bucket& WaitList::bucket_for(const Counter* counter) noexcept
	{
		return buckets[(reinterpret_cast<uintptr_t>(counter) / EMBER_CACHE_LINE) % BUCKET_COUNT];
	}

	// Under the bucket lock the completion that zeroes the counter has either done so
	// already, which the check sees, or has not taken the bucket yet, in which case it finds
	// the link. The counter is alive here: its waiter has not returned.
	bool WaitList::park(FiberRecord* fiber) noexcept
	{
		const Counter* counter = fiber->wait_counter.load(std::memory_order_relaxed);
		Bucket& bucket		   = bucket_for(counter);

		bucket.lock();

		if (counter->is_complete())
		{
			bucket.unlock();
			return false;
		}

		fiber->next_waiter = bucket.head;
		bucket.head		   = fiber;

		bucket.unlock();
		return true;
	}

	// Only the address is compared: the counter may already be gone.
	FiberRecord* WaitList::take(const Counter* counter) noexcept
	{
		Bucket& bucket	   = bucket_for(counter);
		FiberRecord* taken = nullptr;

		bucket.lock();

		for (FiberRecord** link = &bucket.head; *link != nullptr;)
		{
			FiberRecord* fiber = *link;

			if (fiber->wait_counter.load(std::memory_order_relaxed) != counter)
			{
				link = &fiber->next_waiter;
				continue;
			}

			*link			   = fiber->next_waiter;
			fiber->next_waiter = taken;
			taken			   = fiber;
		}

		bucket.unlock();
		return taken;
	}

	void Scheduler::add(Counter& counter, u32 count) noexcept
	{
		counter.m_remaining.fetch_add(count, std::memory_order_relaxed);
	}

	/**
	 * One subtract. The release half publishes the job's writes to whoever reads zero. Past
	 * the subtract the counter is nobody's: a waiter that read zero may have destroyed it,
	 * so the last completion goes on by address alone.
	 */
	void Scheduler::complete(Counter& counter) noexcept
	{
		const u32 before = counter.m_remaining.fetch_sub(1, std::memory_order_acq_rel);
		EMBER_ASSERT(before != 0 && "counter completed past zero");

		if (before != 1)
			return;

		FiberRecord* wake = wait_list.take(&counter);

		while (wake != nullptr)
		{
			FiberRecord* next = wake->next_waiter;
			parked.fetch_sub(1, std::memory_order_relaxed);
			make_ready(wake);
			wake = next;
		}
	}

	// Runs on the fiber that took over from the parking one, so the parked context is
	// complete before anything can resume it.
	void Scheduler::park(FiberRecord* fiber) noexcept
	{
		if (wait_list.park(fiber))
		{
			parked.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		make_ready(fiber);
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

	void FiberPool::init(u32 fiber_count, size_t stack_size, FiberEntry entry) noexcept
	{
		EMBER_ASSERT(records == nullptr && "init runs once");

		count	= fiber_count;
		records = static_cast<FiberRecord*>(
			memory::heap(MemoryTag::Engine).allocate(count * sizeof(FiberRecord), alignof(FiberRecord)));
		free_list.init(queue_capacity(count));

		for (u32 index = 0; index < count; ++index)
		{
			FiberRecord& record = *std::construct_at(records + index);
			record.pool			= true;
			std::snprintf(record.name, sizeof(record.name), "fiber %u", index);

			record.fiber = fiber_create({.stack_size = stack_size, .entry = entry, .arg = &record});
			EMBER_ASSERT(record.fiber != nullptr);

			push_free(&record);
		}
	}

	FiberRecord* FiberPool::pop_free() noexcept
	{
		FiberRecord* fiber = nullptr;
		return free_list.try_pop(fiber) ? fiber : nullptr;
	}

	void FiberPool::push_free(FiberRecord* fiber) noexcept { push_held(free_list, fiber); }

	u32 FiberPool::free_count() const noexcept { return static_cast<u32>(free_list.size_hint()); }

	Scheduler::Scheduler(const JobSystemDef& def) noexcept : def(def)
	{
		const u32 hardware = std::max(1u, std::thread::hardware_concurrency());

		worker_count =
			def.worker_count != 0 ? def.worker_count : hardware - std::min(def.reserved_threads, hardware - 1);

		// Workers and IO threads share the arena's thread slots, which the sleeper mask matches in
		// size, so the IO threads are taken out of the workers' allowance.
		const u32 io_threads = std::min(def.io_threads, IoThreads::MAX_THREADS);
		worker_count		 = std::min(worker_count, Sleepers::MAX_WORKERS - io_threads);
		sleepers.count		 = worker_count;

		// Every worker thread takes a fiber to run its loop on, and main's first wait needs one more.
		fibers.init(std::max(def.fiber_count, worker_count + 1), def.stack_size, fiber_main);
		ready.init(queue_capacity(fibers.count));

		for (MpmcQueue<Job>& queue : jobs)
			queue.init(queue_capacity(def.queue_capacity));

		workers = static_cast<Worker*>(
			memory::heap(MemoryTag::Engine).allocate(worker_count * sizeof(Worker), alignof(Worker)));

		for (u32 index = 0; index < worker_count; ++index)
		{
			Worker& worker	 = *std::construct_at(workers + index);
			worker.scheduler = this;
			worker.index	 = index;
		}

		// Threads start last, once every queue and fiber they may touch exists. Each thread's
		// first fiber is taken here, ahead of a main that may drain the pool before the
		// thread gets to run.
		for (u32 index = 1; index < worker_count; ++index)
		{
			Worker& worker = workers[index];
			worker.first   = fibers.pop_free();
			EMBER_ASSERT(worker.first != nullptr && "out of fibers");

			worker.thread = std::thread([this, index] { worker_main(workers[index]); });
		}

		io.init(*this, io_threads, def.io_queue_capacity);
		adopt_main();
	}

	Scheduler::~Scheduler() noexcept
	{
		// Releasing the adoption undoes a conversion only this thread can undo, and a job on
		// worker 0 would be pulling the scheduler out from under its own fiber.
		EMBER_ASSERT(t_worker == &workers[0] && workers[0].current == &workers[0].thread_record &&
					 "shutdown runs on the thread that initialized, outside any job");

		// The IO threads first: once they are gone, no completion can arrive from outside the workers.
		io.shutdown();

		stopping.store(true, std::memory_order_release);
		sleepers.wake_all();

		for (u32 index = 1; index < worker_count; ++index)
			if (workers[index].thread.joinable())
				workers[index].thread.join();

		const JobStats snapshot = stats();
		EMBER_ASSERT(snapshot.free_fibers == fibers.count && "fibers still parked or running at shutdown");
		EMBER_ASSERT(snapshot.ready_fibers == 0 && "fibers still ready at shutdown");
		EMBER_ASSERT(snapshot.queued_jobs == 0 && "jobs still queued at shutdown");
		(void)snapshot;

		release_main();

		for (u32 index = 0; index < worker_count; ++index)
			std::destroy_at(workers + index);

		memory::heap(MemoryTag::Engine).deallocate(workers, worker_count * sizeof(Worker), alignof(Worker));
	}

	FiberRecord* Scheduler::pop_ready(Worker& worker) noexcept
	{
		// Main is worker 0's alone, and it goes first: the frame is usually waiting on it.
		if (worker.index == 0 && main_ready.load(std::memory_order_relaxed) != nullptr)
			if (FiberRecord* main = main_ready.exchange(nullptr, std::memory_order_acquire))
				return main;

		FiberRecord* fiber = nullptr;
		return ready.try_pop(fiber) ? fiber : nullptr;
	}

	// Anything this worker may continue on: a fiber whose wait completed, else a fresh one.
	FiberRecord* Scheduler::pop_fiber(Worker& worker) noexcept
	{
		FiberRecord* next = pop_ready(worker);
		return next != nullptr ? next : fibers.pop_free();
	}

	void Scheduler::make_ready(FiberRecord* fiber) noexcept
	{
		if (fiber == main_fiber())
		{
			[[maybe_unused]] FiberRecord* previous = main_ready.exchange(fiber, std::memory_order_release);
			EMBER_ASSERT(previous == nullptr && "main made ready twice");
			sleepers.wake_one(0);
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
	 * Switches to next with an action for whoever takes over, and once something switches
	 * back finishes the action that fiber left. Returns the worker this fiber resumed on.
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
		char name[16];
		std::snprintf(name, sizeof(name), "ember.jobs.%u", worker.index);
		const ThreadAttachment attachment(name, ThreadKind::Worker);

		t_worker = &worker;

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
	}

	bool Scheduler::has_work(const Worker& worker) const noexcept
	{
		if (worker.index == 0 && main_ready.load(std::memory_order_relaxed) != nullptr)
			return true;

		if (ready.size_hint() != 0)
			return true;

		for (const MpmcQueue<Job>& queue : jobs)
			if (queue.size_hint() != 0)
				return true;

		return false;
	}

	/**
	 * The epoch is read before the announcement, so a wake-up landing between the announcement
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
	 * Producers touch the mask with a read-modify-write rather than a load: that orders it
	 * after the work they just published, against the sleeper's own read-modify-write. The
	 * clear doubles as the claim: only the producer that saw the bit wakes the worker.
	 */
	void Sleepers::wake_one(u32 index) noexcept
	{
		const u64 bit = u64{1} << index;

		if ((mask.fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0)
			return;

		epochs[index].value.fetch_add(1, std::memory_order_release);
		epochs[index].value.notify_one();
	}

	/** Claims sleepers one bit at a time so several producers do not all wake the same one. */
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

	void Scheduler::kick(Span<const JobDef> decls, Counter* counter) noexcept
	{
		// Counted before any job can start, so no completion can run the count below zero.
		if (counter != nullptr)
			add(*counter, static_cast<u32>(decls.size()));

		for (const JobDef& decl : decls)
		{
			EMBER_ASSERT(decl.fn != nullptr);

			const Job job{.fn = decl.fn, .data = decl.data, .name = decl.name, .counter = counter};

			if (!push_or_full(jobs[static_cast<u32>(decl.priority)], job))
				fail("job queue full, raise JobSystemDef::queue_capacity");
		}

		sleepers.wake_some(static_cast<u32>(decls.size()));
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
			EMBER_PROFILE_FIBER_SCOPE("job");
			EMBER_PROFILE_FIBER_ZONE_NAME(self->job_name, std::strlen(self->job_name));
			job.fn(job.data);
		}

		self->job_name = nullptr;
		trace_split(self);

		if (job.counter != nullptr)
			complete(*job.counter);
	}

	/**
	 * Every pool fiber runs this forever. Ready fibers come first because they hold a stack
	 * and an unfinished job; new jobs start only when nothing is waiting to resume. A fiber
	 * that switches to another one has nothing left to do and frees itself in the process.
	 */
	void Scheduler::loop(FiberRecord* self) noexcept
	{
		Worker* worker = self->worker;
		trace_enter(*worker, self);
		finish_switch(*worker);

		for (;;)
		{
			if (FiberRecord* next = pop_ready(*worker))
			{
				worker = yield_to(self, next, Pending::Free);
				continue;
			}

			Job job;

			if (pop_job(job))
			{
				run_job(self, job);
				worker = self->worker; // a wait inside the job may have moved the fiber
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

	void Scheduler::wait(Counter& counter) noexcept
	{
		EMBER_ASSERT(locks_held() == 0 && "wait with a spin lock held");
		EMBER_ASSERT(!is_io_thread() && "an I/O thread may not wait on the scheduler: nothing would serve its queue");

		Worker* worker = current_worker();

		if (worker == nullptr)
		{
			// A thread outside the scheduler has no fiber to park, so it polls.
			for (u32 spins = 0; !counter.is_complete(); detail::cpu_relax(spins++))
			{
			}

			return;
		}

		FiberRecord* self = worker->current;
		EMBER_ASSERT((self->pool || self == main_fiber()) && "only jobs and main can wait");

		self->wait_counter.store(&counter, std::memory_order_release); // publishes the counter to dumps

		// A wake-up is a hint, as with a futex: a completion that zeroed an earlier counter at
		// this address may wake a fiber parked on this one, which simply parks again.
		while (!counter.is_complete())
		{
			FiberRecord* next = pop_fiber(*worker);
			if (next == nullptr)
				next = stall(self, counter);

			if (next == nullptr)
				break;

			worker = yield_to(self, next, Pending::Park);
		}

		self->wait_counter.store(nullptr, std::memory_order_relaxed);
	}

	/**
	 * Every fiber is parked or busy on another worker. The wait spins for one to come back or
	 * for its own counter to finish. Past stall_report_ms of neither, the pool is too small
	 * for the job graph or the graph has a cycle: the first worker to notice dumps and fails,
	 * the others keep spinning for the moment the failure takes.
	 */
	FiberRecord* Scheduler::stall(FiberRecord* self, Counter& counter) noexcept
	{
		EMBER_PROFILE_SCOPE_C("stalled wait", PROFILE_COLOR_WAIT);
		stalls.fetch_add(1, std::memory_order_relaxed);

		Worker& worker	 = *self->worker;
		const auto since = std::chrono::steady_clock::now();

		for (u32 spin = 0;; detail::cpu_relax(spin++))
		{
			if (counter.is_complete())
				return nullptr;

			if (FiberRecord* next = pop_fiber(worker))
				return next;

			if (def.stall_report_ms != 0 && (spin & 1023) == 1023 &&
				std::chrono::steady_clock::now() - since >= std::chrono::milliseconds(def.stall_report_ms) &&
				!failing.exchange(true, std::memory_order_acq_rel))
			{
				dump("a wait found no fiber and nothing finished");
				fail("a wait found no fiber and nothing finished");
			}
		}
	}

	// Racy by design: a fiber may wake and its counter go away while this runs. It is a
	// diagnostic for a hang, when nothing is moving.
	void Scheduler::dump(const char* reason) noexcept
	{
		const JobStats snapshot = stats();
		EMBER_ERROR("(ember::jobs) {}: {} fibers free, {} parked, {} ready, {} jobs queued, {} io tasks queued", reason,
					snapshot.free_fibers, snapshot.parked_fibers, snapshot.ready_fibers, snapshot.queued_jobs,
					snapshot.queued_io_tasks);

		const auto describe = [this](const FiberRecord& fiber)
		{
			const Counter* counter = fiber.wait_counter.load(std::memory_order_acquire);
			if (counter == nullptr)
				return;

			EMBER_ERROR("(ember::jobs)   {} waits on counter {} with {} left", fiber_label(fiber),
						static_cast<const void*>(counter), counter->m_remaining.load(std::memory_order_relaxed));
		};

		for (u32 index = 0; index < fibers.count; ++index)
			describe(fibers.records[index]);

		describe(*main_fiber());
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

		const u32 main_waiting = main_ready.load(std::memory_order_relaxed) != nullptr ? 1u : 0u;

		return {
			.free_fibers	 = fibers.free_count(),
			.parked_fibers	 = parked.load(std::memory_order_relaxed),
			.ready_fibers	 = static_cast<u32>(ready.size_hint()) + main_waiting,
			.queued_jobs	 = static_cast<u32>(queued),
			.queued_io_tasks = io.queued(),
			.stalls			 = stalls.load(std::memory_order_relaxed),
		};
	}

	// The thread that creates the scheduler is worker 0. Its own stack becomes main's fiber, so
	// main waits like a job does, and main always resumes on this thread: it owns the platform
	// and the device. Runs at the end of construction, on that thread.
	void Scheduler::adopt_main() noexcept
	{
		Worker& worker				= workers[0];
		worker.thread_record.fiber	= fiber_adopt_thread();
		worker.thread_record.worker = &worker;
		worker.current				= &worker.thread_record;
		t_worker					= &worker;
	}

	void Scheduler::release_main() noexcept
	{
		Worker& worker = workers[0];
		t_worker	   = nullptr;
		fiber_release_thread(worker.thread_record.fiber);
		worker.thread_record.fiber = nullptr;
	}
}
