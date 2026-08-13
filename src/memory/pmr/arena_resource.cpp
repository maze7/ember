#include <ember/core/profile.h>
#include <ember/sync/thread.h>
#include <ember/core/bits.h>
#include <ember/memory/pmr/arena_resource.h>
#include <ember/memory/virtual_memory.h>

#include <algorithm>
#include <cstring>

namespace ember
{
	ArenaResource::~ArenaResource() noexcept { shutdown(); }

	bool ArenaResource::init(size_t reserve_size, size_t commit_step, MemoryTag tag) noexcept
	{
		EMBER_ASSERT(m_base == nullptr);
		EMBER_ASSERT(reserve_size != 0);
		EMBER_ASSERT(commit_step != 0);

		if (reserve_size == 0 || commit_step == 0) [[unlikely]]
			return false;

		const size_t rounded_reserve = virtual_memory::round_to_allocation_granularity(reserve_size);
		const size_t rounded_step	 = virtual_memory::round_to_page_size(commit_step);
		void* memory				 = virtual_memory::reserve(rounded_reserve);
		if (memory == nullptr) [[unlikely]]
			return false;

		const size_t initial_commit = std::min(rounded_step, rounded_reserve);
		if (!virtual_memory::commit(memory, initial_commit)) [[unlikely]]
		{
			(void)virtual_memory::release(memory, rounded_reserve);
			return false;
		}

		m_base		   = static_cast<u8*>(memory);
		m_reserved	   = rounded_reserve;
		m_committed	   = initial_commit;
		m_commit_step  = rounded_step;
		m_owner_thread = current_thread_id();
		m_tag		   = tag;
		return true;
	}

	void ArenaResource::shutdown() noexcept
	{
		if (m_base == nullptr)
			return;

		(void)virtual_memory::release(m_base, m_reserved);
		m_base		   = nullptr;
		m_reserved	   = 0;
		m_committed	   = 0;
		m_offset	   = 0;
		m_peak		   = 0;
		m_commit_step  = 0;
		m_owner_thread = 0;
		m_tag		   = MemoryTag::Unknown;
	}

	void ArenaResource::reset(Marker marker) noexcept
	{
		EMBER_ASSERT(current_thread_id() == m_owner_thread);
		EMBER_ASSERT(marker.offset <= m_offset);

		const size_t old_offset = m_offset;
		m_offset				= marker.offset;

		if (m_offset < old_offset)
		{
#if EMBER_MEMORY_TRACKING >= 2
			std::memset(m_base + m_offset, 0xDC, old_offset - m_offset);
#endif
		}
	}

	bool ArenaResource::grow(size_t required) noexcept
	{
		EMBER_PROFILE_SCOPE_C("ArenaResource::grow", profile::COLOR_MEMORY);

		if (required > m_reserved) [[unlikely]]
			return false;

		// m_commit_step is a page multiple, not necessarily a power of two, so no align_up here.
		const size_t deficit	 = required - m_committed;
		const size_t commit_size = ((deficit + m_commit_step - 1) / m_commit_step) * m_commit_step;
		const size_t next_commit = std::min(m_committed + commit_size, m_reserved);
		const size_t grow_size	 = next_commit - m_committed;

		if (grow_size == 0)
			return true;

		if (!virtual_memory::commit(m_base + m_committed, grow_size)) [[unlikely]]
			return false;

		m_committed = next_commit;
		return true;
	}
}
