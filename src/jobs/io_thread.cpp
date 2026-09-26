#include <jobs/scheduler.h>

#include <ember/core/profile.h>
#include <ember/sync/thread.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

namespace ember::jobs
{
	void IoThreads::init(Scheduler& owner, u32 count, u32 queue_capacity) noexcept
	{
		EMBER_ASSERT(threads.empty() && "init runs once");

		scheduler = &owner;

		for (MpmcQueue<Job>& queue : queues)
			queue.init(std::bit_ceil(std::max(queue_capacity, 2u)));

		threads.reserve(count);
		for (u32 index = 0; index < count; ++index)
			threads.emplace_back([this, index] { run(index); });
	}

	void IoThreads::shutdown() noexcept
	{
		// One token per thread: each takes one, finds nothing to run, and leaves.
		stopping.store(true, std::memory_order_release);
		tokens.release(static_cast<std::ptrdiff_t>(threads.size()));

		for (std::thread& thread : threads)
			thread.join();

		threads.clear();
		EMBER_ASSERT(queued() == 0 && "io tasks still queued at shutdown");
	}

	Result<void, IoSubmitError> IoThreads::submit(const IoTask& task, Counter& completion) noexcept
	{
		if (threads.empty())
			return fail(IoSubmitError::NotRunning);

		if (stopping.load(std::memory_order_acquire))
			return fail(IoSubmitError::Stopping);

		// Counted before the task can be seen, so its completion can never run the count below zero.
		scheduler->add(completion, 1);

		const Job job{.fn = task.fn, .data = task.data, .name = task.name, .counter = &completion};

		if (!push_or_full(queues[static_cast<u32>(task.priority)], job))
		{
			// Never seen, so the count goes back the way a completion takes it: through complete(),
			// which wakes the waiters if this was the last thing they were waiting on.
			scheduler->complete(completion);
			return fail(IoSubmitError::QueueFull);
		}

		// In the queue before its token exists, so a thread that takes the token finds it.
		tokens.release();
		return {};
	}

	u32 IoThreads::queued() const noexcept
	{
		size_t total = 0;
		for (const MpmcQueue<Job>& queue : queues)
			total += queue.size_hint();

		return static_cast<u32>(total);
	}

	bool IoThreads::take(Job& job) noexcept
	{
		// The token was released for a task or for shutdown. A task's is in a queue already, though
		// a pop can miss it while another submit is mid push one cell ahead; that push always
		// finishes. Nothing at all means shutdown.
		for (u32 spins = 0;; detail::cpu_relax(spins++))
		{
			for (MpmcQueue<Job>& queue : queues)
				if (queue.try_pop(job))
					return true;

			if (stopping.load(std::memory_order_acquire))
				return false;
		}
	}

	void IoThreads::run(u32 index) noexcept
	{
		char name[16];
		std::snprintf(name, sizeof(name), "ember.io.%u", index);
		const ThreadAttachment attachment(name, ThreadKind::Io);

		Job job;

		for (;;)
		{
			tokens.acquire();

			if (!take(job))
				break;

			{
				EMBER_PROFILE_SCOPE_C("io task", PROFILE_COLOR_IO);
				[[maybe_unused]] const char* label = job.name != nullptr ? job.name : "io task";
				EMBER_PROFILE_ZONE_NAME(label, std::strlen(label));
				job.fn(job.data);
			}

			// The same completion a worker gives a job: the last one wakes whoever parked on it.
			scheduler->complete(*job.counter);
		}
	}
}
