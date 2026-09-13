#include <ember/core/common.h>
#include <ember/core/logger.h>
#include <ember/memory/memory.h>
#include <ember/memory/memory_tracker.h>
#include <ember/memory/pmr/block_allocator.h>
#include <ember/memory/tagged_heap.h>

#include <atomic>
#include <cstdlib>
#include <memory_resource>
#include <source_location>

namespace
{
	constinit ember::TaggedHeap s_block_heap;
	constinit ember::BlockAllocator s_frame_memory;

	// Lifetime kinds. Frame memory is released at the top of every frame; the next kind added here
	// gets 2, and per frame tags pack the frame number into the low half.
	constexpr ember::HeapTag FRAME_TAG		  = ember::heap_tag(1);
	constinit std::atomic<bool> s_initialized = false;

	[[nodiscard]] bool initialize(const ember::MemoryConfig& config) noexcept
	{
		bool expected = false;

		if (!s_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) [[unlikely]]
		{
			EMBER_ASSERT(false && "Only one MemorySystem may exist");
			return false;
		}

		ember::memory::initialize_thread();
		ember::BlockAllocator::register_thread();

#if EMBER_MEMORY_TRACKING >= 2
		if (!ember::memory_tracker::initialize()) [[unlikely]]
		{
			s_initialized.store(false, std::memory_order_release);
			return false;
		}
#endif // EMBER_MEMORY_TRACKING >= 2

		// install_third_party_hooks();
		if (!s_block_heap.init(config.block_heap_capacity, config.block_size, ember::MemoryTag::Engine)) [[unlikely]]
		{
#if EMBER_MEMORY_TRACKING >= 2
			ember::memory_tracker::shutdown();
#endif
			s_initialized.store(false, std::memory_order_release);
			return false;
		}

		s_frame_memory.init(s_block_heap, FRAME_TAG);

		// Last, after every fallible step: from here on, resource-less PMR containers
		// allocate from the engine heap instead of global operator new.
		std::pmr::set_default_resource(&ember::memory::heap(ember::MemoryTag::Unknown));
		return true;
	}

	void shutdown() noexcept
	{
		if (!s_initialized.exchange(false, std::memory_order_acq_rel))
			return;

		s_frame_memory.shutdown();
		s_block_heap.shutdown();
		ember::BlockAllocator::unregister_thread();
#if EMBER_MEMORY_TRACKING >= 2
		const ember::u64 leaks = ember::memory_tracker::report_leaks();
		EMBER_ASSERT(leaks == 0);
		(void)leaks;
		ember::memory_tracker::shutdown();
#endif

		// The default resource deliberately stays on the engine heap; rpmalloc is never
		// finalized (static destructors may free after us), so late allocations remain valid.
	}
}

namespace ember
{
	MemorySystem::MemorySystem(const MemoryConfig& config) noexcept : m_initialized(initialize(config)) {}

	MemorySystem::~MemorySystem() noexcept
	{
		if (m_initialized)
			shutdown();
	}

	TaggedHeap& memory::block_heap() noexcept
	{
		EMBER_ASSERT(s_initialized.load(std::memory_order_acquire));

		return s_block_heap;
	}

	BlockAllocator& memory::frame_memory() noexcept
	{
		EMBER_ASSERT(s_initialized.load(std::memory_order_acquire));

		return s_frame_memory;
	}

	void out_of_memory(size_t size, size_t alignment, MemoryTag tag) noexcept
	{
		Logger::error(std::source_location::current(), "Out of memory: size={} alignment={} tag={}", size, alignment,
					  static_cast<u16>(tag));

		EMBER_DEBUG_BREAK();
		std::abort();
	}
}
