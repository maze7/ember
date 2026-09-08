// include/ember/jobs/job_system.h
#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/core/handle.h>

/**
 * Fiber based job system, heavily inspired by Christian Gyrling's 2015 GDC talk:
 * https://www.gdcvault.com/play/1022186/parallelizing-the-naughty-dog-engine
 *
 * Work is a function plus a data pointer, kicked in batches. A batch owns a counter from
 * the job system's pool: every finished job subtracts one, and a job that needs results
 * waits for the counter to reach a value. The wait parks the job's fiber and the worker
 * thread moves on to other work, so a wait never blocks a core. Every worker runs the same
 * loop inside a fiber: resume fibers whose waits completed first, start new jobs by
 * priority second, and switch straight from fiber to fiber without a scheduler thread.
 *
 * A batch is named by a JobHandle, an index into the pool plus a generation, so a handle
 * kept past the batch's release reads as complete instead of touching whoever reuses the
 * slot. Ownership and waiting are separate: any job may wait on any handle, and only the
 * owner releases it. JobBatch is the owner most code wants: it kicks on construction,
 * waits on destruction and releases, and it moves, so it can live in containers and cross
 * frame stages.
 *
 * run() turns the calling thread into worker 0 and runs main on the thread's own stack.
 * Waits inside main park that stack and always resume it on worker 0, which keeps main on
 * the thread that windowing and GPU APIs demand.
 *
 * Counters are the only synchronisation primitive. A lock may not be held across a wait,
 * because the fiber can resume on another thread. Anything read from thread local storage
 * before a wait is stale after it, worker_index() included; read it again.
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
		JobFn fn			 = nullptr;
		void* data			 = nullptr;
		const char* name	 = nullptr;
		JobPriority priority = JobPriority::Normal;
		JobStack stack		 = JobStack::Small;
	};

	using JobHandle = Handle<JobCounter, u16>;

	struct JobSystemDef
	{
		u32 worker_count	 = 0; // 0 picks one per hardware thread
		u32 small_fibers	 = 128;
		u32 large_fibers	 = 32;
		size_t small_stack	 = 64_kb;
		size_t large_stack	 = 512_kb;
		u32 queue_capacity	 = 4096;  // jobs per priority and stack class
		u32 counter_capacity = 1024;  // batches alive at once
		bool pin_workers	 = false; // lock each worker thread to a core, as on consoles
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

		JobSystem(const JobSystem&)			   = delete;
		JobSystem& operator=(const JobSystem&) = delete;

		/// Makes the calling thread worker 0, runs main there and returns when main returns.
		void run(JobFn main, void* data) noexcept;

	private:
		Impl* m_impl = nullptr;
	};

	/// Kicks the jobs as a new batch and returns its handle. The caller owns the batch and
	/// releases it. Null when the counter pool is exhausted. From any job, main or thread.
	[[nodiscard]] JobHandle kick(Span<const JobDef> jobs) noexcept;
	[[nodiscard]] inline JobHandle kick(const JobDef& job) noexcept { return kick(Span<const JobDef>(&job, 1)); }

	/// Adds jobs to a batch the caller owns.
	void kick(Span<const JobDef> jobs, JobHandle batch) noexcept;
	inline void kick(const JobDef& job, JobHandle batch) noexcept { kick(Span<const JobDef>(&job, 1), batch); }

	/// Kicks jobs nobody will wait for. No counter is allocated.
	void kick_detached(Span<const JobDef> jobs) noexcept;
	inline void kick_detached(const JobDef& job) noexcept { kick_detached(Span<const JobDef>(&job, 1)); }

	/// Parks the calling job or main until the batch's counter is at or below target. From any
	/// other thread it blocks that thread instead. A handle to a batch already released and
	/// finished returns at once.
	void wait(JobHandle batch, u32 target = 0) noexcept;

	/// True once every job in the batch finished, or the batch is gone. Any thread.
	[[nodiscard]] bool is_complete(JobHandle batch) noexcept;

	/// Gives the batch's counter back to the pool: now if the batch finished, otherwise when
	/// its last job does. The jobs keep running either way. Once per batch.
	void release(JobHandle batch) noexcept;

	[[nodiscard]] u32 worker_count() noexcept;

	/// Worker the caller runs on, NO_WORKER on other threads. Stale after a wait.
	[[nodiscard]] u32 worker_index() noexcept;

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
				m_handle	   = other.m_handle;
				other.m_handle = {};
			}

			return *this;
		}

		JobBatch(const JobBatch&)			 = delete;
		JobBatch& operator=(const JobBatch&) = delete;

		[[nodiscard]] JobHandle handle() const noexcept { return m_handle; }
		[[nodiscard]] bool is_complete() const noexcept { return m_handle.is_null() || jobs::is_complete(m_handle); }

		/// Adds jobs, starting the batch if it is empty.
		void kick(Span<const JobDef> jobs) noexcept
		{
			if (m_handle.is_null())
				m_handle = jobs::kick(jobs);
			else
				jobs::kick(jobs, m_handle);
		}

		void kick(const JobDef& job) noexcept { kick(Span<const JobDef>(&job, 1)); }

		void wait(u32 target = 0) noexcept
		{
			if (!m_handle.is_null())
				jobs::wait(m_handle, target);
		}

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
