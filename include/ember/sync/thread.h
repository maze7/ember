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
}
