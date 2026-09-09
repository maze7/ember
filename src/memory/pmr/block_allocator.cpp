#include <ember/memory/memory.h>
#include <ember/memory/pmr/block_allocator.h>

#include <atomic>
#include <bit>

namespace
{
	// One bit per slot, set while a thread holds it. Threads come and go at startup and shutdown,
	// so the mask is only ever contended there.
	constinit std::atomic<ember::u64> s_slots_in_use = 0;

	constinit thread_local ember::u32 t_slot = ember::BlockAllocator::NO_SLOT;

	static_assert(ember::BlockAllocator::MAX_THREADS == 64, "the slot mask holds one bit per slot");
}

namespace ember
{
	BlockAllocator::~BlockAllocator() noexcept { shutdown(); }

	void BlockAllocator::init(TaggedHeap& heap, HeapTag tag) noexcept
	{
		EMBER_ASSERT(m_heap == nullptr);
		EMBER_ASSERT(tag != NO_TAG);

		m_heap = &heap;
		m_tag  = tag;
	}

	void BlockAllocator::shutdown() noexcept
	{
		if (m_heap == nullptr)
			return;

		m_heap->free_blocks(m_tag);
		clear_slots();
		m_heap = nullptr;
		m_tag  = NO_TAG;
	}

	void BlockAllocator::register_thread() noexcept
	{
		EMBER_ASSERT(t_slot == NO_SLOT && "this thread already holds a block slot");

		u64 in_use = s_slots_in_use.load(std::memory_order_relaxed);
		for (;;)
		{
			const u64 available = ~in_use;
			if (available == 0) [[unlikely]]
			{
				EMBER_ASSERT(false && "more frame threads than block slots");
				return;
			}

			const u32 slot = static_cast<u32>(std::countr_zero(available));
			if (s_slots_in_use.compare_exchange_weak(in_use, in_use | (u64{1} << slot), std::memory_order_relaxed))
			{
				t_slot = slot;
				return;
			}
		}
	}

	void BlockAllocator::unregister_thread() noexcept
	{
		if (t_slot == NO_SLOT)
			return;

		s_slots_in_use.fetch_and(~(u64{1} << t_slot), std::memory_order_relaxed);
		t_slot = NO_SLOT;
	}

	u32 BlockAllocator::thread_slot() noexcept { return t_slot; }

	void* BlockAllocator::allocate_from_blocks(Slot& slot, size_t bytes, size_t alignment) noexcept
	{
		EMBER_ASSERT(m_heap != nullptr);

		if (bytes > m_heap->capacity()) [[unlikely]]
			out_of_memory(bytes, alignment, m_heap->memory_tag());

		// Room for the request plus whatever aligning inside the first block costs.
		const size_t block_size = m_heap->block_size();
		const size_t needed		= bytes + alignment - 1;
		const u32 count			= static_cast<u32>((needed + block_size - 1) / block_size);

		auto* base = static_cast<u8*>(m_heap->allocate_blocks(count, m_tag));
		if (base == nullptr) [[unlikely]]
			out_of_memory(bytes, alignment, m_heap->memory_tag());

		u8* aligned = align_up(base, alignment);

		// Whatever was left in the previous block is lost. A thread abandons the tail of one block
		// per refill, which is the price of handing out memory without a lock.
		slot.cursor = aligned + bytes;
		slot.end	= base + static_cast<size_t>(count) * block_size;
		return aligned;
	}

	void BlockAllocator::clear_slots() noexcept
	{
		for (Slot& slot : m_slots)
			slot = {};
	}

	void BlockAllocator::reset() noexcept
	{
		EMBER_ASSERT(m_heap != nullptr);

		m_heap->free_blocks(m_tag);

		// Relaxed, like everything else here. The tag's work has all finished, and the kick that
		// starts the next lot publishes these writes through its counter.
		clear_slots();
	}

	void BlockAllocator::retag(HeapTag tag) noexcept
	{
		EMBER_ASSERT(tag != NO_TAG);

		clear_slots();
		m_tag = tag;
	}
}
