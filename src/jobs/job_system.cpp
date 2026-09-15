#include <ember/jobs/job_system.h>

#include <ember/containers/mpmc_queue.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/block_allocator.h>
#include <ember/sync/spin_mutex.h>
#include <ember/sync/thread.h>
#include <jobs/fiber.h>
#include <jobs/scheduler.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ember::jobs
{
	namespace
	{
		Scheduler* s_scheduler = nullptr;

		// The one way the entry points reach the scheduler.
		Scheduler& scheduler() noexcept
		{
			EMBER_ASSERT(s_scheduler != nullptr && "no job system");
			return *s_scheduler;
		}
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

	void run_main(JobFn main, void* data) noexcept { scheduler().run_main(main, data); }

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
