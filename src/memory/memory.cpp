#include <ember/core/common.h>
#include <ember/core/logger.h>
#include <ember/memory/memory.h>
#include <ember/memory/memory_tracker.h>
#include <ember/memory/pmr/arena.h>
#include <ember/memory/tagged_heap.h>

#include <atomic>
#include <cstdlib>
#include <memory_resource>
#include <source_location>

namespace
{
	constinit ember::TaggedHeap s_block_heap;
	constinit std::atomic<bool> s_initialized = false;
}

namespace ember::memory
{
	Result<void, MemoryError> initialize(const MemoryConfig& config) noexcept
	{
		bool expected = false;

		if (!s_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) [[unlikely]]
		{
			EMBER_ASSERT(false && "Only one MemorySystem may exist");
			return fail(MemoryError::AlreadyInit);
		}

		ember::memory::initialize_thread();
		ember::Arena::register_thread();

#if EMBER_MEMORY_TRACKING >= 2
		if (!ember::memory_tracker::initialize()) [[unlikely]]
		{
			s_initialized.store(false, std::memory_order_release);
			return fail(MemoryError::MemoryTrackerInitFailed);
		}
#endif // EMBER_MEMORY_TRACKING >= 2

		// install_third_party_hooks();
		if (!s_block_heap.init(config.block_heap_capacity, config.block_size)) [[unlikely]]
		{
#if EMBER_MEMORY_TRACKING >= 2
			ember::memory_tracker::shutdown();
#endif
			s_initialized.store(false, std::memory_order_release);
			return fail(MemoryError::BlockHeapInitFailed);
		}

		// Last, after every fallible step: from here on, resource-less PMR containers
		// allocate from the engine heap instead of global operator new.
		std::pmr::set_default_resource(&ember::memory::heap(ember::MemoryTag::Unknown));
		return {};
	}

	void shutdown() noexcept
	{
		if (!s_initialized.exchange(false, std::memory_order_acq_rel))
			return;

		s_block_heap.shutdown();
		ember::Arena::unregister_thread();
#if EMBER_MEMORY_TRACKING >= 2
		const ember::u64 leaks = ember::memory_tracker::report_leaks();
		EMBER_ASSERT(leaks == 0);
		(void)leaks;
		ember::memory_tracker::shutdown();
#endif

		// The default resource deliberately stays on the engine heap; rpmalloc is never
		// finalized (static destructors may free after us), so late allocations remain valid.
	}

	TaggedHeap& tagged_heap() noexcept
	{
		EMBER_ASSERT(s_initialized.load(std::memory_order_acquire));

		return s_block_heap;
	}

	void out_of_memory(size_t size, size_t alignment, MemoryTag tag) noexcept
	{
		Logger::error(std::source_location::current(), "Out of memory: size={} alignment={} tag={}", size, alignment,
					  static_cast<u16>(tag));

		EMBER_DEBUG_BREAK();
		std::abort();
	}
}
