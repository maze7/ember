#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/core/handle.h>

#include <type_traits>

/**
 * Fixed-capacity, fiber-based scheduler for short, CPU-bound engine work.
 *
 * Waiting yields the current fiber instead of blocking its worker, allowing the
 * worker to execute independent jobs while a dependency completes. A suspended
 * job may resume on a different worker, so  jobs must not retain worker-local
 * state, thread-affine resources, or locks needed by the jobs they wait for.
 *
 * Each submitted batch receives one completion handle and one release obligation.
 * Copying a handle does not create additional ownership. The generation embedded
 * in the handle prevents a recycled counter slot from being mistaken for the
 * original batch.
 *
 * Worker 0 remains on the thread that calls run_main() so platform and windowing
 * work can retain the thread affinity required by their APIs.
 *
 * Queue, counter, and fiber capacities are fixed to avoid scheduler allocation
 * during steady-state execution. Exhaustion is treated as a configuration error
 * because silently dropping work would leave dependency counters permanently
 * incomplete.
 *
 * Inspirted by Christian Gyrling's 2015 GDC presentation:
 * https://www.gdcvault.com/play/1022186/parallelizing-the-naughty-dog-engine
 */
namespace ember::jobs
{
	struct JobCounter;

	/**
	 * Type-erased entry point to keep queued jobs allocation-free.
	 * data is borrowed and must remain valid until the invocation returns.
	 */
	using JobFn = void (*)(void* data);

	/** Returned by worker_index() when the caller is not executing on a scheduler worker. */
	inline constexpr u32 NO_WORKER = ~u32{0};

	/**
	 * Controls which queued work workers consider first.
	 *
	 * Priority affects scheduling latency, not execution resources or completion
	 * guarantees. Workers always prefer higher-priority queues, so sustained high
	 * traffic can delay Normal and Low work.
	 *
	 * Reserve High for short jobs that unblock the current frame or a critical
	 * dependency chain. Use Low only for work whose delayed completion cannot
	 * stall frame progress.
	 */
	enum class JobPriority : u8
	{
		High,
		Normal,
		Low,
		Count
	};

	/**
	 * Selects the stack budget reserved for a job.
	 *
	 * Large fibers are deliberately scarce because their reserved address space is
	 * significantly more expensive. Use one only when the job's worst-case stack
	 * demand cannot fit safely within the normal job budget.
	 */
	enum class JobStack : u8
	{
		Small,
		Large,
		Count
	};

	struct JobDef
	{
		JobFn fn			 = nullptr;
		void* data			 = nullptr;
		const char* name	 = nullptr;
		JobPriority priority = JobPriority::Normal;
		JobStack stack		 = JobStack::Small;
	};

	/**
	 * Identifies one generation of a completion-counter  slot.
	 *
	 * A handle returned by submit() carries exactly one release obligation.
	 * Additional copies are borrowed observers and must not outlive that release.
	 */
	using JobHandle = Handle<JobCounter, u32>;

	struct JobSystemDef
	{
		u32 worker_count	 = 0;	   // 0 derives it from the hardware thread count
		u32 reserved_threads = 1;	   // hardware threads left to the OS and engine threads when derived
		u32 small_fibers	 = 128;	   //
		u32 large_fibers	 = 32;	   //
		size_t small_stack	 = 64_kb;  //
		size_t large_stack	 = 512_kb; //
		u32 queue_capacity	 = 4096;   // jobs queued per priority
		u32 counter_capacity = 1024;   // batches alive at once
		u32 stall_report_ms	 = 1000;   // a wait with no fiber and no progress for this long logs the state and fails
		bool pin_workers	 = false;  // lock each worker thread to a core, as on consoles
	};

	/**
	 * The half open index range one job of a parallel_for covers, and that job's
	 * index in the split, for per job partial results.
	 */
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
		JobStack stack		 = JobStack::Small;
	};

	/// A snapshot for debug views. Every count is approximate while jobs run.
	struct JobStats
	{
		u32 free_small_fibers = 0;
		u32 free_large_fibers = 0;
		u32 parked_fibers	  = 0; // waiting on a counter
		u32 ready_fibers	  = 0; // woken, not yet picked up by a worker
		u32 queued_jobs		  = 0;
		u32 live_batches	  = 0;
		u64 stalls			  = 0; // waits that found no fiber
	};

	/**
	 * Initializes the process-wide job scheduler.
	 *
	 * The memory system must already be initialized. This function creates the
	 * fixed-capacity fiber, queue, and counter pools, then starts the background
	 * workers. Worker 0 is reserved for the thread that later calls run_main() and
	 * is not created here.
	 *
	 * Pool capacities remain fixed until shutdown(), and exhausting one is fatal.
	 *
	 * Must be called exactly once before any other jobs API. Initialization and
	 * shutdown must be externally serialized.
	 */
	void initialize(const JobSystemDef& def = {}) noexcept;

	/**
	 * Destroys the process-wide job scheduler.
	 *
	 * run_main() must have returned, every submitted job must be complete, and
	 * every owned JobHandle must have been released. No thread, including a worker,
	 * may enter the jobs API while shutdown is in progress.
	 *
	 * This function wakes and joins the background workers before releasing the
	 * fibers, queues, counters, and scheduler state. All outstanding job handles
	 * are invalid after it returns.
	 */
	void shutdown() noexcept;

	/**
	 * Adopts the calling thread as worker 0 and invokes main.
	 *
	 * The call returns when main returns. Waiting inside main parks its root fiber
	 * while the calling thread executes other work. The root fiber always resumes
	 * on worker 0 so thread-affine platform and GPU operations remain on the calling thread.
	 *
	 * Not re-entrant.
	 */
	void run_main(JobFn main, void* data) noexcept;

	/**
	 * Submits a fixed batch and returns its completion handle.
	 *
	 * The caller owns the handle and must release it exactly once. Empty input
	 * returns a null handle, which is considered complete.
	 */
	[[nodiscard]] JobHandle submit(Span<const JobDef> jobs) noexcept;

	/**
	 * Submits a single job and returns its completion handle.
	 *
	 * The caller owns the handle and must release it exactly once. Empty input
	 * returns a null handle, which is considered complete.
	 */
	[[nodiscard]] inline JobHandle submit(const JobDef& job) noexcept { return submit(Span<const JobDef>(&job, 1)); }

	/// Kicks jobs nobody will wait for. No counter is allocated.
	void submit_detached(Span<const JobDef> jobs) noexcept;
	inline void submit_detached(const JobDef& job) noexcept { submit_detached(Span<const JobDef>(&job, 1)); }

	/// Parks the calling job or main until the batch's last job finishes. From any other
	/// thread it blocks that thread instead. A null handle, or one to a batch already
	/// released and finished, returns at once.
	void wait(JobHandle batch) noexcept;

	/// True once every job in the batch finished, or the batch is gone. Any thread.
	[[nodiscard]] bool is_complete(JobHandle batch) noexcept;

	/// Gives the batch's counter back to the pool: now if the batch finished, otherwise when
	/// its last job does. The jobs keep running either way. Once per batch.
	void release(JobHandle batch) noexcept;

	[[nodiscard]] u32 worker_count() noexcept;

	/// Worker the caller runs on, NO_WORKER on other threads. Stale after a wait.
	[[nodiscard]] u32 worker_index() noexcept;

	[[nodiscard]] JobStats stats() noexcept;

	/// Logs every live batch and the jobs waiting on it, for a hang you are looking at in
	/// the debugger or the console. Any thread.
	void dump_state() noexcept;

	/**
	 * A job over a callable the caller keeps alive until the batch completes: the callable is
	 * the job's data and the thunk invokes it. A local lambda kicked and waited for in the
	 * same scope is the usual shape. The batch stores no copy, so a temporary would dangle.
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
	 * Splits [0, count) into at most MAX_RANGE_JOBS even ranges of about grain items, kicks one
	 * job per range and waits for all of them. Zero items kicks nothing.
	 */
	void parallel_for(const ParallelForDef& def, RangeFn fn, void* data) noexcept;

	/// The callable form; fn(JobRange) is the job body and outlives the call by construction.
	template <class F> void parallel_for(const ParallelForDef& def, F& fn) noexcept
	{
		static_assert(std::is_invocable_v<F&, JobRange>, "job range callables must be invocable and take a JobRange");
		static_assert(!std::is_const_v<F>, "the callable is the job's data and may be mutated");

		parallel_for(def, [](JobRange range, void* data) noexcept { (*static_cast<F*>(data))(range); }, &fn);
	}

	/**
	 * Owns one batch. Kicks in the constructor, waits for the batch on destruction and
	 * releases it. detach() releases without waiting, for jobs whose data outlives the
	 * kicker.
	 */
	class JobBatch final
	{
	public:
		JobBatch() noexcept = default;
		explicit JobBatch(Span<const JobDef> jobs) noexcept : m_handle(jobs::submit(jobs)) {}
		explicit JobBatch(const JobDef& job) noexcept : m_handle(jobs::submit(job)) {}
		~JobBatch() noexcept { reset(); }

		JobBatch(JobBatch&& other) noexcept : m_handle(other.m_handle) { other.m_handle = {}; }

		JobBatch& operator=(JobBatch&& other) noexcept
		{
			if (this != &other)
			{
				reset();
				m_handle	   = other.m_handle;
				other.m_handle = {};
			}

			return *this;
		}

		JobBatch(const JobBatch&)			 = delete;
		JobBatch& operator=(const JobBatch&) = delete;

		[[nodiscard]] JobHandle handle() const noexcept { return m_handle; }
		[[nodiscard]] bool is_complete() const noexcept { return jobs::is_complete(m_handle); }

		void wait() noexcept { jobs::wait(m_handle); }

		/// Releases without waiting. The jobs keep running and the counter frees itself.
		void detach() noexcept
		{
			if (m_handle.is_null())
				return;

			jobs::release(m_handle);
			m_handle = {};
		}

	private:
		void reset() noexcept
		{
			if (m_handle.is_null())
				return;

			jobs::wait(m_handle);
			jobs::release(m_handle);
			m_handle = {};
		}

		JobHandle m_handle;
	};
}
