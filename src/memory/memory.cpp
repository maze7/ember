#include <ember/core/common.h>
#include <ember/core/logger.h>
#include <ember/memory/memory.h>
#include <ember/memory/memory_tracker.h>
#include <ember/memory/pmr/arena_resource.h>

#include <atomic>
#include <cstdlib>
#include <memory_resource>
#include <source_location>

namespace
{
	constinit ember::ArenaResource s_frame_arena;
	constinit std::atomic<bool> s_initialized = false;
}

namespace ember::memory
{
	bool initialize(const MemoryConfig& config) noexcept
	{
		// The allocator layer is process-wide; repeated init calls reuse the existing stae.
		bool expected = false;
		if (!s_initialized.compare_exchange_strong(
				expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
			return true;

		initialize_thread(); // rpmalloc process init; registers the main thread's cache.

#if EMBER_MEMORY_TRACKING >= 2
		if (!memory_tracker::initialize()) [[unlikely]]
		{
			s_initialized.store(false, std::memory_order_release);
			return false;
		}
#endif // EMBER_MEMORY_TRACKING >= 2

		// install_third_party_hooks();
		if (!s_frame_arena.init(config.frame_arena_reserve, config.frame_arena_commit, MemoryTag::Engine)) [[unlikely]]
		{
#if EMBER_MEMORY_TRACKING >= 2
			memory_tracker::shutdown();
#endif
			s_initialized.store(false, std::memory_order_release);
			return false;
		}

		// Last, after every fallible step: from here on, resource-less PMR containers
		// allocate from the engine heap instead of global operator new.
		std::pmr::set_default_resource(&heap(MemoryTag::Unknown));
		return true;
	}

	void shutdown() noexcept
	{
		if (!s_initialized.exchange(false, std::memory_order_acq_rel))
			return;

		s_frame_arena.shutdown();
#if EMBER_MEMORY_TRACKING >= 2
		const u64 leaks = memory_tracker::report_leaks();
		EMBER_ASSERT(leaks == 0);
		(void) leaks;
		memory_tracker::shutdown();
#endif

		// The default resource deliberately stays on the engine heap; rpmalloc is never
		// finalized (static destructors may free after us), so late allocations remain valid.
	}

	ArenaResource& frame_arena() noexcept { return s_frame_arena; }
}

namespace ember
{
	void out_of_memory(size_t size, size_t alignment, MemoryTag tag) noexcept
	{
		Logger::error(std::source_location::current(),
			"Out of memory: size={} alignment={} tag={}",
			size,
			alignment,
			static_cast<u16>(tag)
		);

		EMBER_DEBUG_BREAK();
		std::abort();
	}
}
