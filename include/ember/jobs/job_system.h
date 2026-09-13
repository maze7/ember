#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/core/handle.h>

#include <type_traits>

/**
 * Fiber based job system, heavily inspired by Christian Gyrling's 2015 GDC talk:
 * https://www.gdcvault.com/play/1022186/parallelizing-the-naughty-dog-engine
 *
 * Work is a function plus a data pointer, kicked in batches. A batch owns a counter from
 * the job system's pool: every finished job subtracts one, and a job that needs the results
 * waits for zero. The wait parks the job's fiber and the worker thread moves on to other
 * work, so a wait never blocks a core. Every worker runs the same loop inside a fiber:
 * resume fibers whose waits completed first, start new jobs by priority second, and switch
 * straight from fiber to fiber without a scheduler thread.
 *
 * A batch is fixed at the kick and completes once, when its last job returns. It is named
 * by a JobHandle, an index into the pool plus a generation, so a handle kept past the
 * batch's release reads as complete instead of touching whoever reuses the slot. Ownership
 * and waiting are separate: any job or thread may wait on any handle, and only the owner
 * releases it. JobBatch is the owner most code wants: it kicks on construction, waits on
 * destruction and releases, and it moves, so it can live in containers and cross frame
 * stages.
 *
 * run() turns the calling thread into worker 0 and runs main on the thread's own stack.
 * Waits inside main park that stack and always resume it on worker 0, which keeps main on
 * the thread that windowing and GPU APIs demand.
 *
 * Counters are the only synchronisation primitive. A lock may not be held across a wait,
 * because the fiber can resume on another thread. Anything read from thread local storage
 * before a wait is stale after it, worker_index() included; read it again.
 *
 * Every pool is fixed at construction and sized for the game: jobs queued per priority,
 * batches alive at once, fibers parked at once. Running out of any of them is fatal in
 * every build, like running out of memory, and a wait that finds no fiber logs every live
 * batch and the jobs waiting on it first. Only jobs that wait hold a fiber, so keep fan
 * out wide rather than deep.
 */
namespace ember::jobs
{
	class JobSystem;
	struct JobCounter;

	using JobFn = void (*)(void* data);

	inline constexpr u32 NO_WORKER = ~u32{0};

	enum class JobPriority : u8
	{
		High,
		Normal,
		Low,
		Count
	};

	// Stack the job runs on. Small covers ordinary jobs. Large exists for deep recursion or
	// for handing off to a third party library we do not control, and there are few of them.
	enum class JobStack : u8
	{
		Small,
		Large,
		Count
	};

	struct JobDef
	{
		JobFn fn = nullptr;
		void* data = nullptr;
		const char* name = nullptr;
		JobPriority priority = JobPriority::Normal;
		JobStack stack = JobStack::Small;
	};

	/// Names a batch: a counter slot and the generation it was kicked with.
	using JobHandle = Handle<JobCounter, u32>;

	struct JobSystemDef
	{
		u32 worker_count = 0;	  // 0 derives it from the hardware thread count
		u32 reserved_threads = 1; // hardware threads left to the OS and engine threads when derived
		u32 small_fibers = 128;
		u32 large_fibers = 32;
		size_t small_stack = 64_kb;
		size_t large_stack = 512_kb;
		u32 queue_capacity = 4096;	 // jobs queued per priority
		u32 counter_capacity = 1024; // batches alive at once
		u32 stall_report_ms = 1000;	 // a wait with no fiber and no progress for this long logs the state and fails
		bool pin_workers = false;	 // lock each worker thread to a core, as on consoles
	};

	/// The half open index range one job of a parallel_for covers, and that job's index in the
	/// split, for per job partial results.
	struct JobRange
	{
		u32 begin = 0;
		u32 end = 0;
		u32 index = 0;

		[[nodiscard]] u32 count() const noexcept { return end - begin; }
	};

	using RangeFn = void (*)(JobRange range, void* data);

	/// Jobs one parallel_for may split into. The per job records live on the caller's stack.
	inline constexpr u32 MAX_RANGE_JOBS = 64;

	struct ParallelForDef
	{
		u32 count = 0; // items, indexed [0, count)
		u32 grain = 1; // items one job is worth; sets the job count
		const char* name = nullptr;
		JobPriority priority = JobPriority::Normal;
		JobStack stack = JobStack::Small;
	};

	/// A snapshot for debug views. Every count is approximate while jobs run.
	struct JobStats
	{
		u32 free_small_fibers = 0;
		u32 free_large_fibers = 0;
		u32 parked_fibers = 0; // waiting on a counter
		u32 ready_fibers = 0;  // woken, not yet picked up by a worker
		u32 queued_jobs = 0;
		u32 live_batches = 0;
		u64 stalls = 0; // waits that found no fiber
	};

	/**
	 * Owns the workers, fibers, queues and the counter pool. One per process, created by
	 * the Runtime right after the memory system. The free functions below reach it from
	 * anywhere.
	 */
	class JobSystem final
	{
	public:
		struct Impl;

		explicit JobSystem(const JobSystemDef& def = {}) noexcept;
		~JobSystem() noexcept;

		JobSystem(const JobSystem&) = delete;
		JobSystem& operator=(const JobSystem&) = delete;

		/// Makes the calling thread worker 0, runs main there and returns when main returns.
		void run(JobFn main, void* data) noexcept;

	private:
		Impl* m_impl = nullptr;
	};

	/// Kicks the jobs as a new batch and returns its handle. The caller owns the batch and
	/// releases it. Empty input kicks nothing and returns a null handle, which reads as
	/// complete. From any job, main or thread.
	[[nodiscard]] JobHandle kick(Span<const JobDef> jobs) noexcept;
	[[nodiscard]] inline JobHandle kick(const JobDef& job) noexcept { return kick(Span<const JobDef>(&job, 1)); }

	/// Kicks jobs nobody will wait for. No counter is allocated.
	void kick_detached(Span<const JobDef> jobs) noexcept;
	inline void kick_detached(const JobDef& job) noexcept { kick_detached(Span<const JobDef>(&job, 1)); }

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
		static_assert(std::is_invocable_v<F&>, "job callables take no arguments");
		static_assert(!std::is_const_v<F>, "the callable is the job's data and may be mutated");

		return {
			.fn = [](void* data) { (*static_cast<F*>(data))(); },
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
		static_assert(std::is_invocable_v<F&, JobRange>, "range callables take a JobRange");
		static_assert(!std::is_const_v<F>, "the callable is the job's data and may be mutated");

		parallel_for(def, [](JobRange range, void* data) { (*static_cast<F*>(data))(range); }, &fn);
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
		explicit JobBatch(Span<const JobDef> jobs) noexcept : m_handle(jobs::kick(jobs)) {}
		explicit JobBatch(const JobDef& job) noexcept : m_handle(jobs::kick(job)) {}
		~JobBatch() noexcept { reset(); }

		JobBatch(JobBatch&& other) noexcept : m_handle(other.m_handle) { other.m_handle = {}; }

		JobBatch& operator=(JobBatch&& other) noexcept
		{
			if (this != &other)
			{
				reset();
				m_handle = other.m_handle;
				other.m_handle = {};
			}

			return *this;
		}

		JobBatch(const JobBatch&) = delete;
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
