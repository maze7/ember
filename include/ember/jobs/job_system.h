#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>

#include <atomic>
#include <type_traits>

/**
 * Fixed-capacity, fiber-based scheduler for short, CPU-bound engine work.
 *
 * A job is a function and a pointer. Kicking jobs adds them to a Counter the caller owns,
 * finishing a job subtracts one, and waiting parks the calling fiber until the counter
 * reaches zero while the worker runs other jobs. A parked job may resume on a different
 * worker, so jobs must not keep thread-affine state, worker-local state, or spin locks
 * across a wait.
 *
 * Worker 0 is the thread that calls initialize(), and main's fiber always resumes there, so
 * platform and GPU calls made from main stay on the thread that owns them. Nothing else
 * has a home thread. Work that blocks belongs on a thread outside the scheduler, which
 * reports completion with signal().
 *
 * Fibers and queues are fixed at initialize() so the scheduler never allocates while
 * jobs run. Filling a queue is fatal.
 *
 * Inspired by Christian Gyrling's 2015 GDC presentation:
 * https://www.gdcvault.com/play/1022186/parallelizing-the-naughty-dog-engine
 */
namespace ember::jobs
{
	struct FiberRecord;
	struct Scheduler;
	struct WaitList;

	/** Type-erased entry point. data is borrowed and must outlive the job. */
	using JobFn = void (*)(void* data);

	/** Returned by worker_index() on a thread outside the scheduler */
	inline constexpr u32 NO_WORKER = ~u32{0};

	/**
	 * Which jobs workers drain first. Priority affects latency, not completion. Sustained
	 * High traffic delays Normal and Low work. Reserve High for short jobs that unblock the
	 * frame, and use Low for work whose delay cannot stall the frame.
	 */
	enum class JobPriority : u8
	{
		High,
		Normal,
		Low,
		Count
	};

	struct JobDef
	{
		JobFn fn			 = nullptr;
		void* data			 = nullptr;
		const char* name	 = nullptr;
		JobPriority priority = JobPriority::Normal;
	};

	/**
	 * Work not finished yet, owned by the caller like a latch. submit() adds the jobs it
	 * kicks, each job subtracts itself when it returns, and wait() returns once the count
	 * is zero. Several kicks may share one counter, and a job may kick more work onto the
	 * counter its parent waits on.
	 *
	 * The counter is one atomic integer; the scheduler keeps the fibers waiting on it. It
	 * must outlive every job counted on it and every wait on it. The usual shape is a local:
	 * kick, wait, leave the scope. A counter constructed with a pending count is completed
	 * by signal(), from any thread, for work that finishes outside the scheduler: a read on
	 * the IO thread, a fence the GPU signals, a reply on a socket.
	 */
	class alignas(EMBER_CACHE_LINE) Counter final
	{
	public:
		explicit Counter(u32 pending = 0) noexcept : m_remaining(pending) {}

		~Counter() noexcept
		{
			EMBER_ASSERT(m_remaining.load(std::memory_order_relaxed) == 0 && "counter destroyed with work outstanding");
		}

		Counter(const Counter&)			   = delete;
		Counter& operator=(const Counter&) = delete;

		// True once every completion landed, with the jobs' writes visible. Any thread.
		[[nodiscard]] bool is_complete() const noexcept { return m_remaining.load(std::memory_order_acquire) == 0; }

	private:
		friend struct Scheduler;
		friend struct WaitList;

		std::atomic<u32> m_remaining{0};
	};

	static_assert(sizeof(Counter) == EMBER_CACHE_LINE, "one line per counter, so completions never share one");

	struct JobSystemDef
	{
		u32 worker_count	 = 0;	   // 0 derives it from the hardware thread count
		u32 reserved_threads = 1;	   // hardware threads left to the OS and engine threads when derived
		u32 fiber_count		 = 160;	   // jobs that may be running or parked at once
		size_t stack_size	 = 256_kb; // per fiber; reserved address space, pages commit on first touch
		u32 queue_capacity	 = 4096;   // jobs queued per priority
		u32 stall_report_ms	 = 1000;   // a wait with no fiber and no progress for this long dumps and fails
		bool pin_workers	 = false;  // lock each worker thread to a core
	};

	/** The half-open index range one job of a parallel_for covers, and that job's index in the split */
	struct JobRange
	{
		u32 begin = 0;
		u32 end	  = 0;
		u32 index = 0;

		[[nodiscard]] u32 count() const noexcept { return end - begin; }
	};

	using RangeFn = void (*)(JobRange range, void* data);

	/** Jobs one parallel_for may split into. The per job records live on the caller's stack. */
	inline constexpr u32 MAX_RANGE_JOBS = 64;

	struct ParallelForDef
	{
		u32 count			 = 0; // items, indexed [0, count)
		u32 grain			 = 1; // items one job is worth; sets the job count
		const char* name	 = nullptr;
		JobPriority priority = JobPriority::Normal;
	};

	/** A snapshot for debug views. Every count is approximate while jobs run. */
	struct JobStats
	{
		u32 free_fibers	  = 0;
		u32 parked_fibers = 0; // waiting on a counter
		u32 ready_fibers  = 0; // woken, not yet picked up by a worker
		u32 queued_jobs	  = 0;
		u64 stalls		  = 0; // waits that found no fiber
	};

	/**
	 * Creates the fiber pool, queues and starts the background workers. The calling thread
	 * becomes Worker 0. The memory system must already be up.
	 */

	/**
	 * Creates the fiber pool, queues and starts the background workers. The memory system
	 * must already be up. Call once per process, serialized against shutdown(). The calling
	 * thread becomes Worker 0.
	 */
	void initialize(const JobSystemDef& def = {}) noexcept;

	/** Joins the workers and releases the fibers and queues. Every kicked job must be complete */
	void shutdown() noexcept;

	/**
	 * Kicks jobs. With a counter, every job is added to it before any can start and
	 * subtracts itself when it finishes. Without one the jobs are fire and forget.
	 */
	void kick(Span<const JobDef> jobs, Counter* counter = nullptr) noexcept;

	inline void kick(const JobDef& job, Counter* counter = nullptr) noexcept
	{
		kick(Span<const JobDef>(&job, 1), counter);
	}

	/**
	 * Parks the calling job or main until the counter reaches zero. A thread outside the
	 * scheduler has no fiber to park, so it polls instead.
	 */
	void wait(Counter& counter) noexcept;

	/**
	 * One completion from outside the scheduler. Signal exactly as many times as the pending
	 * count; the last one wakes every waiter.
	 */
	void signal(Counter& counter) noexcept;

	[[nodiscard]] u32 worker_count() noexcept;

	/** Worker the caller runs on, NO_WORKER on other threads. Stale after a wait. */
	[[nodiscard]] u32 worker_index() noexcept;

	[[nodiscard]] JobStats stats() noexcept;

	/** Logs every fiber waiting on a counter, for a hang you are looking at. Any thread. */
	void dump_state() noexcept;

	/**
	 * A job over a callable the caller keeps alive until the job completes: the callable is
	 * the job's data and the thunk invokes it. A local lambda kicked and waited for in the
	 * same scope is the usual shape. Nothing is copied, so a temporary would dangle.
	 */
	template <class F> [[nodiscard]] JobDef make_job(F& fn, const char* name = nullptr) noexcept
	{
		static_assert(std::is_invocable_v<F&>, "job callables must be invocable and take no arguments");
		static_assert(!std::is_const_v<F>, "the callable is the job's data and may be mutated");

		return {
			.fn	  = [](void* data) noexcept { (*static_cast<F*>(data))(); },
			.data = &fn,
			.name = name,
		};
	}

	/**
	 * Splits [0, count) into at most MAX_RANGE_JOBS even ranges of about grain items, kicks
	 * one job per range and waits for all of them. Zero items kicks nothing.
	 */
	void parallel_for(const ParallelForDef& def, RangeFn fn, void* data) noexcept;

	/** The callable form; fn(JobRange) is the job body and outlives the call by construction. */
	template <class F> void parallel_for(const ParallelForDef& def, F& fn) noexcept
	{
		static_assert(std::is_invocable_v<F&, JobRange>, "job range callables must be invocable and take a JobRange");
		static_assert(!std::is_const_v<F>, "the callable is the job's data and may be mutated");

		parallel_for(def, [](JobRange range, void* data) noexcept { (*static_cast<F*>(data))(range); }, &fn);
	}

	/**
	 * A counter that kicks in the constructor and waits in the destructor. The jobs' data
	 * must outlive the batch, which a local batch guarantees for locals declared above it.
	 */
	class Batch final
	{
	public:
		explicit Batch(Span<const JobDef> jobs) noexcept { jobs::kick(jobs, &m_counter); }
		explicit Batch(const JobDef& job) noexcept { jobs::kick(job, &m_counter); }
		~Batch() noexcept { wait(); }

		Batch(const Batch&)			   = delete;
		Batch& operator=(const Batch&) = delete;

		void wait() noexcept { jobs::wait(m_counter); }
		[[nodiscard]] bool is_complete() const noexcept { return m_counter.is_complete(); }

		// For kicking more work onto the same batch.
		[[nodiscard]] Counter& counter() noexcept { return m_counter; }

	private:
		Counter m_counter;
	};
}
