#pragma once

#include <ember/core/common.h>

namespace ember
{
	/**
	 * Much faster than OS thread IDs and perfect for array indexing in thread-local allocators.
	 *
	 * Never inlined: a job resumes on whichever worker picks it up, so a thread local read the
	 * optimiser hoisted out of a wait would hand back the thread the job started on.
	 */
	EMBER_NOINLINE u32 current_thread_id();

	/// Names the calling thread for debuggers and profilers. Linux keeps 15 characters.
	void set_thread_name(const char* name) noexcept;

	/// Pins the calling thread to one logical core. False where unsupported or refused.
	bool set_thread_affinity(u32 core) noexcept;

#if EMBER_LOCK_TRACKING
	/**
	 * Debug count of the spin locks the calling thread holds. A spin lock is taken and released
	 * on one thread with no wait between, so this is useful to make sure a fiber doesn't own
	 * any locks when switching.
	 */
	EMBER_NOINLINE void note_lock_taken() noexcept;
	EMBER_NOINLINE void note_lock_released() noexcept;
	EMBER_NOINLINE u32 locks_held() noexcept;

	#define EMBER_LOCK_TAKEN() ::ember::note_lock_taken()
	#define EMBER_LOCK_RELEASED() ::ember::note_lock_released()
#else
	#define EMBER_LOCK_TACKEN() ((void)0)
	#define EMBER_LOCK_RELEASED() ((void)0)
#endif
}
