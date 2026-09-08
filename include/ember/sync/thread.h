#pragma once

#include <ember/core/common.h>

namespace ember
{
	/**
	 * Returns a fast, engine-assigned sequential integer ID for the current thread (1, 2, 3...).
	 * Much faster than OS thread IDs and perfect for array indexing in thread-local allocators.
	 */
	u32 current_thread_id();

	/// Names the calling thread for debuggers and profilers. Linux keeps 15 characters.
	void set_thread_name(const char* name) noexcept;

	/// Pins the calling thread to one logical core. False where unsupported or refused.
	bool set_thread_affinity(u32 core) noexcept;
}
