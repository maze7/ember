#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/memory/memory.h>

#include <atomic>
#include <memory>

namespace ember
{
	/**
	 * Fixed capacity set of slot indices that remembers which ones changed, so a consumer walks
	 * what moved instead of the whole table.
	 *
	 * One bit per slot deduplicates and a dense list carries the marks. Marking is safe from every
	 * frame thread at once, the same slot included: the bit is the claim, and exactly one marker
	 * finds it clear and appends.
	 *
	 * Reading and clearing are not. slots() and clear() belong to the phase after the markers
	 * finish, which is a wait on the counter that ended their batch, and that wait is what
	 * publishes the marks. Everything here is relaxed for the same reason.
	 *
	 * Marks land in whatever order the threads got there, so a consumer that wants contiguous runs
	 * sorts a copy of the span.
	 */
	class DirtySet
	{
	public:
		explicit DirtySet(MemoryTag tag = MemoryTag::Unknown) noexcept : m_resource(&memory::heap(tag)) {}
		explicit DirtySet(std::pmr::memory_resource& resource) noexcept : m_resource(&resource) {}

		~DirtySet() noexcept
		{
			if (m_block == nullptr)
				return;

			std::destroy_n(m_bits, word_count(m_capacity));
			m_resource->deallocate(m_block, m_block_size, alignof(std::atomic<u64>));
		}

		DirtySet(const DirtySet&) = delete;
		DirtySet& operator=(const DirtySet&) = delete;

		/// Sizes the set once. Capacity is the number of slots, not of marks: every slot can be
		/// marked once, so the list can never overflow.
		void init(u32 capacity) noexcept
		{
			EMBER_ASSERT(m_block == nullptr && "init runs once");
			EMBER_ASSERT(capacity != 0);

			const size_t bits_bytes = word_count(capacity) * sizeof(std::atomic<u64>);

			m_block_size = bits_bytes + capacity * sizeof(u32);
			m_block = m_resource->allocate(m_block_size, alignof(std::atomic<u64>));
			m_bits = static_cast<std::atomic<u64>*>(m_block);
			m_list = reinterpret_cast<u32*>(static_cast<std::byte*>(m_block) + bits_bytes);
			m_capacity = capacity;

			for (size_t i = 0; i < word_count(capacity); ++i)
				std::construct_at(&m_bits[i], u64{0});
		}

		/// Marks the slot changed. Any frame thread, any slot, at any time before the phase ends.
		void mark(u32 slot) noexcept
		{
			EMBER_ASSERT(slot < m_capacity);

			std::atomic<u64>& word = m_bits[slot >> 6];
			const u64 bit = u64{1} << (slot & 63);

			if ((word.fetch_or(bit, std::memory_order_relaxed) & bit) != 0)
				return;

			const u32 index = m_count.fetch_add(1, std::memory_order_relaxed);
			EMBER_ASSERT(index < m_capacity);

			m_list[index] = slot;
		}

		/// The marked slots, in the order they were claimed. Valid once the markers are joined.
		[[nodiscard]] Span<const u32> slots() const noexcept
		{
			return {m_list, m_count.load(std::memory_order_relaxed)};
		}

		[[nodiscard]] u32 size() const noexcept { return m_count.load(std::memory_order_relaxed); }
		[[nodiscard]] bool empty() const noexcept { return size() == 0; }
		[[nodiscard]] u32 capacity() const noexcept { return m_capacity; }

		/// Empties the set. Same phase as slots().
		void clear() noexcept
		{
			const u32 count = m_count.load(std::memory_order_relaxed);

			// Sparse reset: a steady frame marks a handful of slots, and a memset would touch the
			// whole bitset for them.
			for (u32 i = 0; i < count; ++i)
				m_bits[m_list[i] >> 6].fetch_and(~(u64{1} << (m_list[i] & 63)), std::memory_order_relaxed);

			m_count.store(0, std::memory_order_relaxed);
		}

	private:
		[[nodiscard]] static constexpr size_t word_count(u32 capacity) noexcept { return (capacity + 63) / 64; }

		std::atomic<u64>* m_bits = nullptr;
		u32* m_list = nullptr;
		std::atomic<u32> m_count = 0;
		u32 m_capacity = 0;

		std::pmr::memory_resource* m_resource = nullptr;
		void* m_block = nullptr;
		size_t m_block_size = 0;
	};
}
