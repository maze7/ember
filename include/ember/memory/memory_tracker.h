#pragma once

#include <ember/core/common.h>
#include <ember/memory/common.h>

/// Level 1 (Profile): global heap counters (current/peak/count) + Tracy zones. Relaxed atomics only.
/// Level 2 (Debug): adds per-tag attribution, budget warnings, the live-pointer leak map, and poisoning.
#if EMBER_MEMORY_TRACKING >= 1
namespace ember::memory_tracker
{
	struct TagStats
	{
		u64 current_bytes = 0;
		u64 peak_bytes	  = 0;
		u64 current_count = 0;
		u64 total_count	  = 0;
	};

	[[nodiscard]] bool initialize() noexcept;
	void shutdown() noexcept;

	void on_alloc(void* ptr, size_t size, MemoryTag tag, bool poison) noexcept;
	void on_free(void* ptr, size_t size) noexcept;
	void on_free_unpoisoned(void* ptr, size_t size) noexcept;

	[[nodiscard]] TagStats total() noexcept;
	void report(bool as_csv = false) noexcept;

	#if EMBER_MEMORY_TRACKING >= 2
	[[nodiscard]] TagStats stats(MemoryTag tag) noexcept;
	[[nodiscard]] u64 report_leaks() noexcept;
	void break_on_allocation(u64 allocation_id) noexcept;
	#endif // EMBER_MEMORY_TRACKING
}

	#define EMBER_MEM_TRACK_ALLOC(ptr, size, tag, poison)                                                              \
		::ember::memory_tracker::on_alloc((ptr), (size), (tag), (poison))
	#define EMBER_MEM_TRACK_FREE(ptr, size) ::ember::memory_tracker::on_free((ptr), (size))
	#define EMBER_MEM_TRACK_FREE_UNPOISONED(ptr, size) ::ember::memory_tracker::on_free_unpoisoned((ptr), (size))
#else
	#define EMBER_MEM_TRACK_ALLOC(ptr, size, tag, poison) ((void)0)
	#define EMBER_MEM_TRACK_FREE(ptr, size) ((void)0)
	#define EMBER_MEM_TRACK_FREE_UNPOISONED(ptr, size) ((void)0)
#endif
