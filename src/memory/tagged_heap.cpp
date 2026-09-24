#include <ember/core/bits.h>
#include <ember/core/profile.h>
#include <ember/memory/memory.h>
#include <ember/memory/tagged_heap.h>
#include <ember/memory/virtual_memory.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>

namespace ember
{
	namespace
	{
		constexpr u32 NO_BLOCK = ~u32{0};
	}

	TaggedHeap::~TaggedHeap() noexcept { shutdown(); }

	bool TaggedHeap::init(size_t capacity, size_t block_size) noexcept
	{
		EMBER_ASSERT(m_base == nullptr);
		EMBER_ASSERT(is_power_of_two(block_size));
		EMBER_ASSERT(capacity >= block_size);

		if (m_base != nullptr || !is_power_of_two(block_size) || capacity < block_size) [[unlikely]]
			return false;

		// Rounding to the granularity keeps whole blocks. Both sizes are powers of two,
		// so the larger one is a multiple of the smaller.
		const size_t rounded = virtual_memory::round_to_allocation_granularity(align_up(capacity, block_size));
		const size_t blocks	 = rounded / block_size;

		if (blocks > 0x00FF'FFFFu) [[unlikely]] // sixteen million blocks; the bookkeeping is not built for more
			return false;

		void* memory = virtual_memory::reserve(rounded);
		if (memory == nullptr) [[unlikely]]
			return false;

		// Committed here, all of it, so taking a block is a few loads unser a short lock and never
		// a system call. Physical pages still arrive on first touch.
		if (!virtual_memory::commit(memory, rounded)) [[unlikely]]
		{
			(void)virtual_memory::release(memory, rounded);
			return false;
		}

		const u32 words = static_cast<u32>((blocks + 63) / 64);
		Heap& heap		= memory::heap(TaggedHeap::MEMORY_TAG);

		m_free = static_cast<u64*>(heap.allocate(words * sizeof(u64), alignof(u64)));
		m_tags = static_cast<std::atomic<HeapTag>*>(
			heap.allocate(blocks * sizeof(std::atomic<HeapTag>), alignof(std::atomic<HeapTag>)));

		for (u32 word = 0; word < words; ++word)
			m_free[word] = 0;

		for (u32 block = 0; block < blocks; ++block)
		{
			m_free[block / 64] |= u64{1} << (block % 64);
			std::construct_at(&m_tags[block], NO_TAG);
		}

		m_base		  = static_cast<u8*>(memory);
		m_capacity	  = rounded;
		m_block_size  = block_size;
		m_block_count = static_cast<u32>(blocks);
		m_word_count  = words;
		m_hint		  = 0;
		return true;
	}

	void TaggedHeap::shutdown() noexcept
	{
		if (m_base == nullptr)
			return;

		EMBER_ASSERT(blocks_in_use() == 0 && "a tag still owns blocks at showdown; every lifetime must be freed first");

		(void)virtual_memory::release(m_base, m_capacity);

		for (u32 block = 0; block < m_block_count; ++block)
			std::destroy_at(&m_tags[block]);

		Heap& heap = memory::heap(TaggedHeap::MEMORY_TAG);
		heap.deallocate(m_tags, m_block_count * sizeof(std::atomic<HeapTag>), alignof(std::atomic<HeapTag>));
		heap.deallocate(m_free, m_word_count * sizeof(u64), alignof(u64));

		m_free		  = nullptr;
		m_tags		  = nullptr;
		m_base		  = nullptr;
		m_capacity	  = 0;
		m_block_size  = 0;
		m_block_count = 0;
		m_word_count  = 0;
		m_hint		  = 0;
		m_blocks_in_use.store(0, std::memory_order_relaxed);
		m_peak_blocks.store(0, std::memory_order_relaxed);
	}

	u32 TaggedHeap::find_run(u32 count) const noexcept
	{
		// The common case is one block: the first set bit of the first non empty word, starting where
		// the last scan proved everything below to be full.
		if (count == 1)
		{
			for (u32 word = m_hint; word < m_word_count; ++word)
				if (m_free[word] != 0)
					return word * 64 + static_cast<u32>(std::countr_zero(m_free[word]));

			return NO_BLOCK;
		}

		// A run has to be consecutive, so walk the bits and count. Bits past block_count are never set,
		// which bounds to the walk without a second limit. Cold: 99% of requests are one block.
		u32 run = 0;
		for (u32 block = m_hint * 64; block < m_block_count; ++block)
		{
			if (!is_free(block))
			{
				run = 0;
				continue;
			}

			if (++run == count)
				return block + 1 - count;
		}

		return NO_BLOCK;
	}

	void* TaggedHeap::allocate(u32 count, HeapTag tag) noexcept
	{
		EMBER_PROFILE_SCOPE_C("TaggedHeap::allocate", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(m_base != nullptr);
		EMBER_ASSERT(tag != NO_TAG);
		EMBER_ASSERT(count != 0);

		if (count == 0 || count > m_block_count) [[unlikely]]
			return nullptr;

		u32 first = NO_BLOCK;

		m_lock.lock();

		first = find_run(count);
		if (first != NO_BLOCK)
		{
			for (u32 block = first; block < first + count; ++block)
			{
				m_free[block / 64] &= ~(u64{1} << (block % 64));
				m_tags[block].store(tag, std::memory_order_relaxed);
			}

			if (count == 1)
				m_hint = first / 64;
		}

		m_lock.unlock();

		if (first == NO_BLOCK) [[unlikely]]
			return nullptr;

		const u32 in_use = m_blocks_in_use.fetch_add(count, std::memory_order_relaxed) + count;
		u32 peak		 = m_peak_blocks.load(std::memory_order_relaxed);
		while (in_use > peak && !m_peak_blocks.compare_exchange_weak(peak, in_use, std::memory_order_relaxed))
			;

		return m_base + static_cast<size_t>(first) * m_block_size;
	}

	u32 TaggedHeap::free(HeapTag tag) noexcept
	{
		EMBER_PROFILE_SCOPE_C("TaggedHeap::free", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(m_base != nullptr);
		EMBER_ASSERT(tag != NO_TAG);

#if EMBER_MEMORY_TRACKING >= 2
		// Before the lock: the blocks are still this tag's, so nobody else can take them, and a
		// memset the size of a block has no business under a spin lock. A stale pointer then reads
		// the pattern instead of last frame's data.
		for (u32 block = 0; block < m_block_count; ++block)
			if (m_tags[block].load(std::memory_order_relaxed) == tag)
				std::memset(m_base + static_cast<size_t>(block) * m_block_size, 0xDC, m_block_size);
#endif

		u32 freed  = 0;
		u32 lowest = m_word_count;

		m_lock.lock();

		for (u32 block = 0; block < m_block_count; ++block)
		{
			if (m_tags[block].load(std::memory_order_relaxed) != tag)
				continue;

			m_tags[block].store(NO_TAG, std::memory_order_relaxed);
			m_free[block / 64] |= u64{1} << (block % 64);
			lowest = std::min(lowest, block / 64);
			++freed;
		}

		// Pull the scan back to the lowest word this tag returned, so the heap refills from the
		// bottom instead of drifting upwards and free runs stay contiguous for longer.
		if (lowest < m_hint)
			m_hint = lowest;

		m_lock.unlock();
		m_blocks_in_use.fetch_sub(freed, std::memory_order_relaxed);
		return freed;
	}

	HeapTag TaggedHeap::tag_of(const void* ptr) const noexcept
	{
		if (!owns(ptr))
			return NO_TAG;

		const size_t block = static_cast<size_t>(static_cast<const u8*>(ptr) - m_base) / m_block_size;
		return m_tags[block].load(std::memory_order_relaxed);
	}

	u32 TaggedHeap::blocks_owned(HeapTag tag) const noexcept
	{
		u32 owned = 0;
		for (u32 block = 0; block < m_block_count; ++block)
			owned += m_tags[block].load(std::memory_order_relaxed) == tag;

		return owned;
	}
}
