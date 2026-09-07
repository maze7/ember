#pragma once

#include <ember/core/common.h>

/**
 * Fiber seam of the job system: a stack plus the registers needed to resume code on it.
 * A switch runs entirely on the calling thread, so a job can park mid function and the
 * worker moves on to other work within nanoseconds.
 *
 * Contracts:
 *  - The entry function never returns. A fiber ends by switching away for the last time
 *	  and its owner destroys it afterwards.
 *	- A fiber runs on at most one thread at a time and is never switched to while running.
 *	- An adopted thread's fiber holds no state until the first switch away from it, so it
 *	  can only be a switch source until then.
 *	- Destroy and release never target the running fiber.
 *
 * Hand written context switch on x86_64 and AArch64 (Linux, macOS), Win32 fibers on Windows.
 */
namespace ember::jobs
{
	struct Fiber;

	using FiberEntry = void (*)(void* arg);

	struct FiberDef
	{
		size_t stack_size = 64_kb;
		FiberEntry entry  = nullptr;
		void* arg		  = nullptr;
	};

	// Allocates a guard paged stack of at least stack_size bytes. Null on allocation failure.
	[[nodiscard]] Fiber* fiber_create(const FiberDef& def) noexcept;
	void fiber_destroy(Fiber* fiber) noexcept;

	// Wraps the calling thread's own stack so it can take part in switches.
	[[nodiscard]] Fiber* fiber_adopt_thread() noexcept;
	void fiber_release_thread(Fiber* fiber) noexcept;

	// Saves the running state into from and resumes to. Returns once something switches back.
	void fiber_switch(Fiber* from, Fiber* to) noexcept;
}
