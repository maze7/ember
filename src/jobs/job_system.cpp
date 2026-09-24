#include <ember/jobs/job_system.h>

#include <ember/memory/memory.h>
#include <jobs/scheduler.h>

#include <algorithm>

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
		memory::delete_object(MemoryTag::Engine, s_scheduler);
		s_scheduler = nullptr;
	}

	void kick(Span<const JobDef> jobs, Counter* counter) noexcept
	{
		if (!jobs.empty())
			scheduler().kick(jobs, counter);
	}

	void wait(Counter& counter) noexcept { scheduler().wait(counter); }

	void signal(Counter& counter) noexcept { scheduler().complete(counter); }

	u32 worker_count() noexcept { return scheduler().worker_count; }

	u32 worker_index() noexcept
	{
		const Worker* worker = current_worker();
		return worker != nullptr ? worker->index : NO_WORKER;
	}

	bool is_main() noexcept
	{
		const Worker* worker = current_worker();
		return worker != nullptr && worker->current == scheduler().main_fiber();
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
			defs[i]	  = {.fn = run_range, .data = &ranges[i], .name = def.name, .priority = def.priority};

			begin = end;
		}

		Counter done;
		kick(Span<const JobDef>(defs, jobs), &done);
		wait(done);
	}
}
