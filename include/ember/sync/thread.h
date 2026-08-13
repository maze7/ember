#pragma once

#include <ember/core/common.h>

namespace ember
{
	/**
	 * Returns a fast, engine-assigned sequential integer ID for the current thread (1, 2, 3...).
	 * Much faster than OS thread IDs and perfect for array indexing in thread-local allocators.
	 */
	u32 current_thread_id();
}
