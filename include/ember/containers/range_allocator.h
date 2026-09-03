#pragma once

#include <ember/core/common.h>
#include <ember/memory/memory.h>

#include <algorithm>

namespace ember
{
	/**
	 * Free-range bookkeeping for suballocated element spaces (mesh vertices in a
	 * shared buffer, index ranges, table slots). Tracks the free set only; callers
	 * keep their own {offset, count} and hand both back on free, so there is no
	 * per-allocation metadata here.
	 *
	 * The free list stays address sorted and fully coalesced, and allocation is
	 * first fit rom low offsets. Churn is asset-frequency (chunk rebuilds, level
	 * loads), so a linear scan over a small vector wins on simplicity and cache
	 * behaviour; we can swap to a bucketed allocator only if a profile determines
	 * it is needed.
	 *
	 * Offsets and counts are elements. Byte scaling belongs to the owner, which
	 * knows its stride.
	 *
	 * Not thread-safe. init() runs once; before it every allocate fails.
	 */
	class RangeAllocator
	{
	public:
		static constexpr u32 INVALID_OFFSET = ~0u;

		struct Range
		{
			u32 offset = 0;
			u32 count  = 0;
		};

		explicit RangeAllocator(MemoryTag tag = MemoryTag::Unknown) noexcept : m_free(&memory::heap(tag)) {}

		RangeAllocator(const RangeAllocator&)			 = delete;
		RangeAllocator& operator=(const RangeAllocator&) = delete;

		void init(u32 capacity) noexcept
		{
			EMBER_ASSERT(m_capacity == 0 && "init runs once");
			EMBER_ASSERT(capacity != 0);

			m_capacity = capacity;
			m_free.reserve(64);
			m_free.push_back({0, capacity});
		}

		/// First fit. INVALID_OFFSET when no free range holds `count` elements.
		[[nodiscard]] u32 allocate(u32 count) noexcept
		{
			EMBER_ASSERT(count != 0);

			for (size_t i = 0; i < m_free.size(); ++i)
			{
				Range& range = m_free[i];
				if (range.count < count)
					continue;

				const u32 offset = range.offset;

				if (range.count == count)
				{
					m_free.erase(m_free.begin() + static_cast<ptrdiff_t>(i));
				}
				else
				{
					range.offset += count;
					range.count	 -= count;
				}

				m_used += count;
				return offset;
			}

			return INVALID_OFFSET;
		}

		/// `offset` and `count` must name exactly one prior allocate. Merges with
		/// both neighbours so the free list never holds adjacent ranges.
		void free(u32 offset, u32 count) noexcept
		{
			EMBER_ASSERT(count != 0 && offset + count <= m_capacity);
			EMBER_ASSERT(m_used >= count);

			m_used -= count;

			const auto next = std::lower_bound(
				m_free.begin(),
				m_free.end(),
				offset,
				[](const Range& range, u32 value) { return range.offset < value; });

			const bool has_prev = next != m_free.begin();
			const auto prev		= has_prev ? next - 1 : m_free.end();

			// A freed range that touches live free space means a double free or a
			// mismatched count; both corrupt the space silently in release.
			EMBER_ASSERT(!has_prev || prev->offset + prev->count <= offset);
			EMBER_ASSERT(next == m_free.end() || offset + count <= next->offset);

			const bool joins_prev = has_prev && prev->offset + prev->count == offset;
			const bool joins_next = next != m_free.end() && offset + count == next->offset;

			if (joins_prev && joins_next)
			{
				prev->count += count + next->count;
				m_free.erase(next);
			}
			else if (joins_prev)
			{
				prev->count += count;
			}
			else if (joins_next)
			{
				next->offset  = offset;
				next->count	 += count;
			}
			else
			{
				m_free.insert(next, {offset, count});
			}
		}

		[[nodiscard]] u32 capacity() const noexcept { return m_capacity; }
		[[nodiscard]] u32 used() const noexcept { return m_used; }

		/// Free-list entries; a fragmentation signal for stats displays.
		[[nodiscard]] u32 range_count() const noexcept { return static_cast<u32>(m_free.size()); }

	private:
		Vector<Range> m_free;
		u32 m_capacity = 0;
		u32 m_used	   = 0;
	};
}
