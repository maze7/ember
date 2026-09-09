#include <ember/core/profile.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/frame_resource.h>
#include <ember/memory/virtual_memory.h>

#include <bit>
#include <cstring>

namespace
{
	// One bit per slot, set while a thread holds it. Threads come and go at startup and shutdown,
	// so the mask is only ever contended there.
	constinit std::atomic<ember::u64> s_slots_in_use = 0;

	constinit thread_local ember::u32 t_slot = ember::FrameResource::NO_SLOT;

	static_assert(ember::FrameResource::MAX_THREADS == 64, "the slot mask holds one bit per slot");
}

namespace ember
{
	FrameResource::~FrameResource() noexcept { shutdown(); }

	bool FrameResource::init(size_t capacity, size_t block_size, MemoryTag tag) noexcept
	{
		EMBER_ASSERT(m_base == nullptr);
		EMBER_ASSERT(is_power_of_two(block_size));
		EMBER_ASSERT(capacity >= block_size);

		if (m_base != nullptr || !is_power_of_two(block_size) || capacity < block_size) [[unlikely]]
			return false;

		// Rounding to the granularity keeps whole blocks: both sizes are powers of two, so the
		// larger one is a multiple of the smaller.
		const size_t rounded = virtual_memory::round_to_allocation_granularity(align_up(capacity, block_size));

		void* memory = virtual_memory::reserve(rounded);
		if (memory == nullptr) [[unlikely]]
			return false;

		// The whole budget is committed here, so taking a block is one atomic add and never a
		// system call. Untouched pages still cost nothing but address space.
		if (!virtual_memory::commit(memory, rounded)) [[unlikely]]
		{
			(void)virtual_memory::release(memory, rounded);
			return false;
		}

		m_base		  = static_cast<u8*>(memory);
		m_capacity	  = rounded;
		m_block_size  = block_size;
		m_block_count = static_cast<u32>(rounded / block_size);
		m_tag		  = tag;
		return true;
	}

	void FrameResource::shutdown() noexcept
	{
		if (m_base == nullptr)
			return;

		(void)virtual_memory::release(m_base, m_capacity);

		for (Slot& slot : m_slots)
			slot = {};

		m_base		  = nullptr;
		m_capacity	  = 0;
		m_block_size  = 0;
		m_block_count = 0;
		m_peak_blocks = 0;
		m_next_block.store(0, std::memory_order_relaxed);
		m_tag = MemoryTag::Unknown;
	}

	void FrameResource::register_thread() noexcept
	{
		EMBER_ASSERT(t_slot == NO_SLOT && "this thread already holds a frame slot");

		u64 in_use = s_slots_in_use.load(std::memory_order_relaxed);
		for (;;)
		{
			const u64 available = ~in_use;
			if (available == 0) [[unlikely]]
			{
				EMBER_ASSERT(false && "more frame threads than frame slots");
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

	void FrameResource::unregister_thread() noexcept
	{
		if (t_slot == NO_SLOT)
			return;

		s_slots_in_use.fetch_and(~(u64{1} << t_slot), std::memory_order_relaxed);
		t_slot = NO_SLOT;
	}

	u32 FrameResource::thread_slot() noexcept { return t_slot; }

	void* FrameResource::allocate_from_blocks(Slot& slot, size_t bytes, size_t alignment) noexcept
	{
		if (bytes > m_capacity) [[unlikely]]
			out_of_memory(bytes, alignment, m_tag);

		// Room for the request plus whatever aligning inside the first block costs.
		const size_t needed = bytes + alignment - 1;
		const u32 count		= static_cast<u32>((needed + m_block_size - 1) / m_block_size);
		const u32 first		= m_next_block.fetch_add(count, std::memory_order_relaxed);

		if (first + count > m_block_count) [[unlikely]]
			out_of_memory(bytes, alignment, m_tag);

		u8* base	= m_base + static_cast<size_t>(first) * m_block_size;
		u8* aligned = align_up(base, alignment);

		// Whatever was left in the previous block is lost. A thread abandons the tail of one block
		// per refill, which is the price of handing out memory without a lock.
		slot.cursor = aligned + bytes;
		slot.end	= base + static_cast<size_t>(count) * m_block_size;
		return aligned;
	}

	void FrameResource::reset() noexcept
	{
		EMBER_PROFILE_SCOPE_C("FrameResource::reset", PROFILE_COLOR_MEMORY);

		const u32 used = m_next_block.load(std::memory_order_relaxed);
		m_peak_blocks  = used > m_peak_blocks ? used : m_peak_blocks;

#if EMBER_MEMORY_TRACKING >= 2
		std::memset(m_base, 0xDC, static_cast<size_t>(used) * m_block_size);
#endif

		for (Slot& slot : m_slots)
			slot = {};

		// Relaxed throughout. The frame's jobs have all finished, and the kick that starts the next
		// frame's work publishes these writes through its counter.
		m_next_block.store(0, std::memory_order_relaxed);
	}
