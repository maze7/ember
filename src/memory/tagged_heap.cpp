#include <ember/core/bits.h>
#include <ember/core/profile.h>
#include <ember/memory/memory.h>
#include <ember/memory/tagged_heap.h>
#include <ember/memory/virtual_memory.h>

#include <cstring>
#include <memory>

namespace ember
{
	TaggedHeap::~TaggedHeap() noexcept { shutdown(); }

	bool TaggedHeap::init(size_t capacity, size_t block_size, MemoryTag tag) noexcept
	{
		EMBER_ASSERT(m_base == nullptr);
		EMBER_ASSERT(is_power_of_two(block_size));
		EMBER_ASSERT(capacity >= block_size);

		if (m_base != nullptr || !is_power_of_two(block_size) || capacity < block_size) [[unlikely]]
			return false;

		// Rounding to the granularity keeps whole blocks: both sizes are powers of two, so the
		// larger one is a multiple of the smaller.
		const size_t rounded = virtual_memory::round_to_allocation_granularity(align_up(capacity, block_size));
		const u32 blocks	 = static_cast<u32>(rounded / block_size);

		void* memory = virtual_memory::reserve(rounded);
		if (memory == nullptr) [[unlikely]]
			return false;

		// The whole pool is committed here, so taking a block is a few atomics and never a system
		// call. Untouched pages still cost nothing but address space.
		if (!virtual_memory::commit(memory, rounded)) [[unlikely]]
		{
			(void)virtual_memory::release(memory, rounded);
			return false;
		}

		m_block_tags = static_cast<std::atomic<HeapTag>*>(
			memory::heap(tag).allocate(blocks * sizeof(std::atomic<HeapTag>), alignof(std::atomic<HeapTag>)));

		for (u32 i = 0; i < blocks; ++i)
			std::construct_at(&m_block_tags[i], NO_TAG);

		m_base		  = static_cast<u8*>(memory);
		m_capacity	  = rounded;
		m_block_size  = block_size;
		m_block_count = blocks;
		m_tag		  = tag;
		return true;
	}

	void TaggedHeap::shutdown() noexcept
	{
		if (m_base == nullptr)
			return;

		(void)virtual_memory::release(m_base, m_capacity);

		for (u32 i = 0; i < m_block_count; ++i)
			std::destroy_at(&m_block_tags[i]);

		memory::heap(m_tag).deallocate(m_block_tags, m_block_count * sizeof(std::atomic<HeapTag>),
									   alignof(std::atomic<HeapTag>));

		m_block_tags  = nullptr;
		m_base		  = nullptr;
		m_capacity	  = 0;
		m_block_size  = 0;
		m_block_count = 0;
		m_hint.store(0, std::memory_order_relaxed);
		m_blocks_in_use.store(0, std::memory_order_relaxed);
		m_peak_blocks.store(0, std::memory_order_relaxed);
		m_tag = MemoryTag::Unknown;
	}

	bool TaggedHeap::window_is_free(u32 first, u32 count) const noexcept
	{
		for (u32 i = 0; i < count; ++i)
			if (m_block_tags[first + i].load(std::memory_order_relaxed) != NO_TAG)
				return false;

		return true;
	}

	u32 TaggedHeap::claim(u32 first, u32 count, HeapTag tag) noexcept
	{
		for (u32 i = 0; i < count; ++i)
		{
			HeapTag expected = NO_TAG;

			// Acquire pairs with the release in release()/free_blocks(), so the block's contents
			// are ours to overwrite once the tag is.
			if (!m_block_tags[first + i].compare_exchange_strong(expected, tag, std::memory_order_acquire,
																 std::memory_order_relaxed))
				return i;
		}

		return count;
	}

	void TaggedHeap::release(u32 first, u32 count) noexcept
	{
		for (u32 i = 0; i < count; ++i)
			m_block_tags[first + i].store(NO_TAG, std::memory_order_release);
	}

	void* TaggedHeap::allocate_blocks(u32 count, HeapTag tag) noexcept
	{
		EMBER_PROFILE_SCOPE_C("TaggedHeap::allocate_blocks", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(m_base != nullptr);
		EMBER_ASSERT(tag != NO_TAG);
		EMBER_ASSERT(count != 0);

		if (count == 0 || count > m_block_count) [[unlikely]]
			return nullptr;

		const u32 windows = m_block_count - count + 1;

		// A sweep that only ever lost races has not proven the heap is full, so it sweeps again.
		// Every lost race is another thread's claim succeeding, which is what makes this lock free.
		for (;;)
		{
			const u32 hint = m_hint.load(std::memory_order_relaxed);
			bool lost_race = false;

			for (u32 offset = 0; offset < windows; ++offset)
			{
				const u32 first = (hint + offset) % windows;
				if (!window_is_free(first, count))
					continue;

				const u32 taken = claim(first, count, tag);
				if (taken == count)
				{
					m_hint.store((first + count) % windows, std::memory_order_relaxed);

					const u32 in_use = m_blocks_in_use.fetch_add(count, std::memory_order_relaxed) + count;
					u32 peak		 = m_peak_blocks.load(std::memory_order_relaxed);
					while (in_use > peak &&
						   !m_peak_blocks.compare_exchange_weak(peak, in_use, std::memory_order_relaxed))
						;

					return m_base + static_cast<size_t>(first) * m_block_size;
				}

				// Somebody took a block out from under the scan. Give back the part of the run we
				// did claim and keep looking.
				release(first, taken);
				lost_race = true;
			}

			if (!lost_race)
				return nullptr;
		}
	}

	void TaggedHeap::free_blocks(HeapTag tag) noexcept
	{
		EMBER_PROFILE_SCOPE_C("TaggedHeap::free_blocks", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(tag != NO_TAG);

		u32 freed  = 0;
		u32 lowest = m_block_count;

		for (u32 i = 0; i < m_block_count; ++i)
		{
			if (m_block_tags[i].load(std::memory_order_relaxed) != tag)
				continue;

#if EMBER_MEMORY_TRACKING >= 2
			std::memset(m_base + static_cast<size_t>(i) * m_block_size, 0xDC, m_block_size);
#endif

			m_block_tags[i].store(NO_TAG, std::memory_order_release);
			lowest = lowest < i ? lowest : i;
			++freed;
		}

		m_blocks_in_use.fetch_sub(freed, std::memory_order_relaxed);

		// Pull the scan back to the lowest block this tag returned, so the pool refills from the
		// bottom instead of drifting upwards. A frame that allocates the same way twice then gets
		// the same addresses twice, and free runs stay contiguous for longer.
		u32 hint = m_hint.load(std::memory_order_relaxed);
		while (lowest < hint && !m_hint.compare_exchange_weak(hint, lowest, std::memory_order_relaxed))
			;
	}
}
