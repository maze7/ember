#pragma once

#include "ember/containers/pool.h"

namespace ember
{
	template <class Tag, class Hot, class Cold> template <bool IsConst> class Pool<Tag, Hot, Cold>::SlotIterator
	{
	public:
		using iterator_category = std::forward_iterator_tag;
		using iterator_concept	= std::forward_iterator_tag;
		using value_type		= Hot;
		using difference_type	= std::ptrdiff_t;
		using pointer			= std::conditional_t<IsConst, const Hot*, Hot*>;
		using reference			= std::conditional_t<IsConst, const Hot&, Hot&>;

		SlotIterator() = default;

		[[nodiscard]] EMBER_FINLINE reference operator*() const noexcept { return m_pool->m_hot[m_index]; }

		[[nodiscard]] EMBER_FINLINE pointer operator->() const noexcept { return m_pool->m_hot + m_index; }

		[[nodiscard]] EMBER_FINLINE HandleType handle() const noexcept { return m_pool->make_handle(m_index); }

		EMBER_FINLINE SlotIterator& operator++() noexcept
		{
			++m_index;
			skip_dead();
			return *this;
		}

		EMBER_FINLINE SlotIterator operator++(int) noexcept
		{
			SlotIterator copy = *this;
			++*this;
			return copy;
		}

		[[nodiscard]] bool operator==(const SlotIterator&) const noexcept = default;

	private:
		friend Pool;

		using PoolPointer = std::conditional_t<IsConst, const Pool*, Pool*>;

		SlotIterator(PoolPointer pool, u32 index) noexcept : m_pool(pool), m_index(index) { skip_dead(); }

		EMBER_FINLINE void skip_dead() noexcept
		{
			while (m_index < m_pool->m_capacity && m_pool->m_slots[m_index].state != OCCUPIED)
				++m_index;
		}

		PoolPointer m_pool = nullptr;
		u32 m_index		   = 0;
	};

	template <class Tag, class Hot, class Cold>
	Pool<Tag, Hot, Cold>::Pool(MemoryTag tag) noexcept : Pool(memory::heap(tag), tag)
	{
	}

	template <class Tag, class Hot, class Cold>
	Pool<Tag, Hot, Cold>::Pool(std::pmr::memory_resource& resource, MemoryTag diagnostics_tag) noexcept
		: m_resource(&resource), m_tag(diagnostics_tag)
	{
	}

	template <class Tag, class Hot, class Cold> Pool<Tag, Hot, Cold>::~Pool() noexcept { release(); }

	template <class Tag, class Hot, class Cold>
	Pool<Tag, Hot, Cold>::Pool(Pool&& other) noexcept : m_resource(other.m_resource), m_tag(other.m_tag)
	{
		take_storage(other);
	}

	template <class Tag, class Hot, class Cold> auto Pool<Tag, Hot, Cold>::operator=(Pool&& other) noexcept -> Pool&
	{
		if (this == &other)
			return *this;

		release();

		m_resource = other.m_resource;
		m_tag	   = other.m_tag;

		take_storage(other);
		return *this;
	}

	template <class Tag, class Hot, class Cold>
	template <class... Args>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::emplace(Args&&... args) noexcept -> HandleType
	{
		// Growth is handled separately because it must materialize arguments
		// before reserve() when they may reference pool storage. The rare
		// O(capacity) path is not force-inlined to avoid bloating hot call sites.
		if (m_size == m_capacity) [[unlikely]]
			return emplace_grow(std::forward<Args>(args)...);

		// The common, allocation-free path is small and force-inlinable.
		return emplace_no_grow(std::forward<Args>(args)...);
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::insert(Hot&& value) noexcept -> HandleType
	{
		return emplace(std::move(value));
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::insert(const Hot& value) noexcept -> HandleType
	{
		return emplace(value);
	}

	template <class Tag, class Hot, class Cold>
	template <class HotArg, class ColdArg>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::insert(HotArg&& hot, ColdArg&& cold) noexcept -> HandleType
		requires(HAS_COLD)
	{
		// Growth is handled separately because it must materialize arguments
		// before reserve() when they may reference pool storage. The rare
		// O(capacity) path is not force-inlined to avoid bloating hot call sites.
		if (m_size == m_capacity) [[unlikely]]
		{
			return insert_grow(std::forward<HotArg>(hot), std::forward<ColdArg>(cold));
		}

		// The common, allocation-free path is small and force-inlinable.
		return insert_no_grow(std::forward<HotArg>(hot), std::forward<ColdArg>(cold));
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE bool Pool<Tag, Hot, Cold>::erase(HandleType handle) noexcept
	{
		if (!contains(handle))
			return false;

		const u32 index			  = static_cast<u32>(handle.index);
		const bool had_free_slots = m_size < m_capacity;

		retire(index);

		// Push onto the intrusive LIFO list only after destruction. When this
		// was the only free slot its state value is never read.
		Slot& slot = m_slots[index];

		slot.state = had_free_slots ? static_cast<u16>(m_free_head) : 0;

		m_free_head = index;
		--m_size;

		return true;
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE bool Pool<Tag, Hot, Cold>::contains(HandleType handle) const noexcept
	{
		if (handle.is_null() || handle.index >= m_capacity) [[unlikely]]
			return false;

		const Slot& slot = m_slots[handle.index];

		return slot.state == OCCUPIED && slot.generation == handle.generation;
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::try_get(HandleType handle) noexcept -> Hot*
	{
		return contains(handle) ? m_hot + handle.index : nullptr;
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::try_get(HandleType handle) const noexcept -> const Hot*
	{
		return contains(handle) ? m_hot + handle.index : nullptr;
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::get(HandleType handle) noexcept -> Hot&
	{
		EMBER_ASSERT(contains(handle));
		return m_hot[handle.index];
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::get(HandleType handle) const noexcept -> const Hot&
	{
		EMBER_ASSERT(contains(handle));
		return m_hot[handle.index];
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::try_get_cold(HandleType handle) noexcept -> ColdType*
		requires(HAS_COLD)
	{
		return contains(handle) ? m_cold + handle.index : nullptr;
	}
	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::try_get_cold(HandleType handle) const noexcept -> const ColdType*
		requires(HAS_COLD)
	{
		return contains(handle) ? m_cold + handle.index : nullptr;
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::get_cold(HandleType handle) noexcept -> ColdType&
		requires(HAS_COLD)
	{
		EMBER_ASSERT(contains(handle));
		return m_cold[handle.index];
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::get_cold(HandleType handle) const noexcept -> const ColdType&
		requires(HAS_COLD)
	{
		EMBER_ASSERT(contains(handle));
		return m_cold[handle.index];
	}

	template <class Tag, class Hot, class Cold> EMBER_FINLINE u32 Pool<Tag, Hot, Cold>::size() const noexcept
	{
		return m_size;
	}

	template <class Tag, class Hot, class Cold> EMBER_FINLINE u32 Pool<Tag, Hot, Cold>::capacity() const noexcept
	{
		return m_capacity;
	}

	template <class Tag, class Hot, class Cold> EMBER_FINLINE bool Pool<Tag, Hot, Cold>::empty() const noexcept
	{
		return m_size == 0;
	}

	template <class Tag, class Hot, class Cold> void Pool<Tag, Hot, Cold>::reserve(u32 requested_capacity) noexcept
	{
		if (requested_capacity <= m_capacity)
			return;

		if (requested_capacity > MAX_CAPACITY) [[unlikely]]
			fail_allocation(requested_capacity);

		u32 new_capacity = m_capacity < MIN_CAPACITY ? MIN_CAPACITY : m_capacity;

		while (new_capacity < requested_capacity)
		{
			if (new_capacity > MAX_CAPACITY / 2)
			{
				new_capacity = MAX_CAPACITY;
				break;
			}

			new_capacity *= 2;
		}

		reallocate(new_capacity);
	}

	template <class Tag, class Hot, class Cold> void Pool<Tag, Hot, Cold>::clear() noexcept
	{
		retire_all();
		rebuild_free_list();
	}

	template <class Tag, class Hot, class Cold> EMBER_FINLINE auto Pool<Tag, Hot, Cold>::begin() noexcept -> Iterator
	{
		return Iterator(this, 0);
	}

	template <class Tag, class Hot, class Cold> EMBER_FINLINE auto Pool<Tag, Hot, Cold>::end() noexcept -> Iterator
	{
		return Iterator(this, m_capacity);
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::begin() const noexcept -> ConstIterator
	{
		return ConstIterator(this, 0);
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::end() const noexcept -> ConstIterator
	{
		return ConstIterator(this, m_capacity);
	}

	template <class Tag, class Hot, class Cold>
	template <class... Args>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::emplace_no_grow(Args&&... args) noexcept -> HandleType
	{
		const u32 index = acquire_slot();

		std::construct_at(m_hot + index, std::forward<Args>(args)...);

		if constexpr (HAS_COLD)
			std::construct_at(m_cold + index);

		return publish_slot(index);
	}

	template <class Tag, class Hot, class Cold>
	template <class... Args>
	auto Pool<Tag, Hot, Cold>::emplace_grow(Args&&... args) noexcept -> HandleType
	{
		if (m_capacity >= MAX_CAPACITY) [[unlikely]]
			fail_allocation(std::numeric_limits<size_t>::max());

		// Materialize before reserve() so arguments may safely reference
		// existing pool values. Only growth pays for this extra move.
		Hot pending_hot(std::forward<Args>(args)...);

		if constexpr (HAS_COLD)
		{
			ColdType pending_cold{};
			reserve(m_capacity + 1);
			return insert_no_grow(std::move(pending_hot), std::move(pending_cold));
		}
		else
		{
			reserve(m_capacity + 1);
			return emplace_no_grow(std::move(pending_hot));
		}
	}

	template <class Tag, class Hot, class Cold>
	template <class HotArg, class ColdArg>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::insert_no_grow(HotArg&& hot, ColdArg&& cold) noexcept -> HandleType
		requires(HAS_COLD)
	{
		const u32 index = acquire_slot();

		std::construct_at(m_hot + index, std::forward<HotArg>(hot));
		std::construct_at(m_cold + index, std::forward<ColdArg>(cold));

		return publish_slot(index);
	}

	template <class Tag, class Hot, class Cold>
	template <class HotArg, class ColdArg>
	auto Pool<Tag, Hot, Cold>::insert_grow(HotArg&& hot, ColdArg&& cold) noexcept -> HandleType
		requires(HAS_COLD)
	{
		if (m_capacity >= MAX_CAPACITY) [[unlikely]]
			fail_allocation(std::numeric_limits<size_t>::max());

		Hot pending_hot(std::forward<HotArg>(hot));
		ColdType pending_cold(std::forward<ColdArg>(cold));

		reserve(m_capacity + 1);

		return insert_no_grow(std::move(pending_hot), std::move(pending_cold));
	}

	template <class Tag, class Hot, class Cold> EMBER_FINLINE u32 Pool<Tag, Hot, Cold>::acquire_slot() noexcept
	{
		EMBER_ASSERT(m_size < m_capacity);
		EMBER_ASSERT(m_free_head != INVALID_INDEX);

		const u32 free_count = m_capacity - m_size;
		const u32 index		 = m_free_head;
		Slot& slot			 = m_slots[index];

		EMBER_ASSERT(slot.state != OCCUPIED);

		if (free_count > 1)
		{
			const u32 next = static_cast<u32>(slot.state);
			EMBER_ASSERT(next < m_capacity);
			EMBER_ASSERT(next != index);
			m_free_head = next;
		}
		else
		{
			m_free_head = INVALID_INDEX;
		}

		// Acquired but not yet published as a live slot.
		slot.state = 0;
		return index;
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::publish_slot(u32 index) noexcept -> HandleType
	{
		EMBER_ASSERT(index < m_capacity);

		Slot& slot = m_slots[index];

		EMBER_ASSERT(slot.state != OCCUPIED);
		EMBER_ASSERT(slot.generation != 0);

		slot.state = OCCUPIED;
		++m_size;

		return make_handle(index);
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold>::make_handle(u32 index) const noexcept -> HandleType
	{
		EMBER_ASSERT(index < m_capacity);
		EMBER_ASSERT(m_slots[index].state == OCCUPIED);
		EMBER_ASSERT(m_slots[index].generation != 0);

		return {
			static_cast<u16>(index),
			m_slots[index].generation,
		};
	}

	template <class Tag, class Hot, class Cold>
	EMBER_FINLINE void Pool<Tag, Hot, Cold>::advance_generation(Slot& slot) noexcept
	{
		u16 generation = static_cast<u16>(slot.generation + 1);
		if (generation == 0)
			generation = 1;

		slot.generation = generation;
	}

	template <class Tag, class Hot, class Cold> void Pool<Tag, Hot, Cold>::retire(u32 index) noexcept
	{
		EMBER_ASSERT(index < m_capacity);
		EMBER_ASSERT(m_slots[index].state == OCCUPIED);

		// Invalidate before invoking user destructors.
		m_slots[index].state = 0;
		advance_generation(m_slots[index]);

		if constexpr (HAS_COLD)
			std::destroy_at(m_cold + index);
		std::destroy_at(m_hot + index);
	}

	template <class Tag, class Hot, class Cold> void Pool<Tag, Hot, Cold>::retire_all() noexcept
	{
		for (u32 index = 0; index < m_capacity && m_size != 0; ++index)
		{
			if (m_slots[index].state != OCCUPIED)
				continue;

			retire(index);
			--m_size;
		}

		EMBER_ASSERT(m_size == 0);
	}

	template <class Tag, class Hot, class Cold> void Pool<Tag, Hot, Cold>::rebuild_free_list() noexcept
	{
		m_free_head	   = INVALID_INDEX;
		u32 free_count = 0;

		// Build 0 -> 1 -> 2 ... so index zero is acquired first.
		for (u32 index = m_capacity; index-- > 0;)
		{
			Slot& slot = m_slots[index];

			EMBER_ASSERT(slot.state != OCCUPIED);

			slot.state = free_count != 0 ? static_cast<u16>(m_free_head) : u16{0};

			m_free_head = index;
			++free_count;
		}

		EMBER_ASSERT(free_count == m_capacity);
	}

	template <class Tag, class Hot, class Cold>
	[[noreturn]] void Pool<Tag, Hot, Cold>::fail_allocation(size_t requested_size) const noexcept
	{
		out_of_memory(requested_size, BLOCK_ALIGNMENT, m_tag);
	}

	template <class Tag, class Hot, class Cold>
	size_t Pool<Tag, Hot, Cold>::carve(size_t& cursor, size_t count, size_t stride, size_t alignment) const noexcept
	{
		EMBER_ASSERT(is_power_of_two(alignment));

		size_t bytes = 0;
		if (!checked_mul(count, stride, bytes)) [[unlikely]]
			fail_allocation(std::numeric_limits<size_t>::max());

		const size_t mask = alignment - 1;

		if (cursor > std::numeric_limits<size_t>::max() - mask) [[unlikely]]
			fail_allocation(std::numeric_limits<size_t>::max());

		const size_t offset = align_up(cursor, alignment);

		if (!checked_add(offset, bytes, cursor)) [[unlikely]]
			fail_allocation(std::numeric_limits<size_t>::max());

		return offset;
	}

	template <class Tag, class Hot, class Cold>
	auto Pool<Tag, Hot, Cold>::make_layout(u32 capacity) const noexcept -> Layout
	{
		const size_t count = capacity;
		size_t cursor	   = 0;
		Layout layout{};

		// The allocation itself aligns the beginning of the hot stream.
		(void)carve(cursor, count, sizeof(Hot), BLOCK_ALIGNMENT);

		if constexpr (HAS_COLD)
			layout.cold_offset = carve(cursor, count, sizeof(ColdType), BLOCK_ALIGNMENT);

		// Metadata starts on a separate cache-line boundary.
		layout.slots_offset = carve(cursor, count, sizeof(Slot), BLOCK_ALIGNMENT);
		layout.total_size	= cursor;
		return layout;
	}

	template <class Tag, class Hot, class Cold>
	void Pool<Tag, Hot, Cold>::relocate_values(Hot* new_hot, ColdType* new_cold) noexcept
	{
		(void)new_cold;

		// Trivial streams use one bulk copy. Dead bytes remain unreachable and
		// copying them avoids a branch per slot during growth.
		if constexpr (std::is_trivially_copyable_v<Hot>)
			std::memcpy(new_hot, m_hot, static_cast<size_t>(m_capacity) * sizeof(Hot));

		if constexpr (HAS_COLD && std::is_trivially_copyable_v<ColdType>)
			std::memcpy(new_cold, m_cold, static_cast<size_t>(m_capacity) * sizeof(ColdType));

		// If either stream is non-trivial, scan liveness once and relocate all
		// non-trivial streams together.
		if constexpr (!std::is_trivially_copyable_v<Hot> || (HAS_COLD && !std::is_trivially_copyable_v<ColdType>))
		{
			for (u32 index = 0; index < m_capacity; ++index)
			{
				if (m_slots[index].state != OCCUPIED)
					continue;

				if constexpr (!std::is_trivially_copyable_v<Hot>)
				{
					std::construct_at(new_hot + index, std::move(m_hot[index]));
					std::destroy_at(m_hot + index);
				}

				if constexpr (HAS_COLD && !std::is_trivially_copyable_v<ColdType>)
				{
					std::construct_at(new_cold + index, std::move(m_cold[index]));
					std::destroy_at(m_cold + index);
				}
			}
		}
	}

	template <class Tag, class Hot, class Cold> void Pool<Tag, Hot, Cold>::reallocate(u32 new_capacity) noexcept
	{
		EMBER_ASSERT(new_capacity > m_capacity);
		EMBER_ASSERT(new_capacity <= MAX_CAPACITY);

		const u32 old_capacity	 = m_capacity;
		const u32 old_free_count = old_capacity - m_size;
		const Layout layout		 = make_layout(new_capacity);

		void* new_block	   = m_resource->allocate(layout.total_size, BLOCK_ALIGNMENT);
		auto* new_base	   = static_cast<std::byte*>(new_block);
		Hot* new_hot	   = reinterpret_cast<Hot*>(new_base);
		ColdType* new_cold = nullptr;

		if constexpr (HAS_COLD)
			new_cold = reinterpret_cast<ColdType*>(new_base + layout.cold_offset);

		Slot* new_slots = reinterpret_cast<Slot*>(new_base + layout.slots_offset);

		if (old_capacity != 0)
		{
			relocate_values(new_hot, new_cold);

			// memcpy begins the lifetime of trivially copyable Slot objects.
			std::memcpy(new_slots, m_slots, static_cast<size_t>(old_capacity) * sizeof(Slot));
		}

		// Fresh slots begin free at generation one.
		for (u32 index = old_capacity; index < new_capacity; ++index)
			std::construct_at(new_slots + index);

		// Prefix fresh sslots before the previous free list:
		//
		// fresh slots -> previous free slots
		//
		// Automatic growth occurs only when the old pool is full. For explicit
		// reserve(), consuming fresh slots first delays generation recycling.
		u32 new_free_head  = m_free_head;
		u32 new_free_count = old_free_count;

		for (u32 index = new_capacity; index-- > old_capacity;)
		{
			Slot& slot	  = new_slots[index];
			slot.state	  = new_free_count != 0 ? static_cast<u16>(new_free_head) : 0;
			new_free_head = index;
			++new_free_count;
		}

		if (m_block != nullptr)
			m_resource->deallocate(m_block, m_block_size, BLOCK_ALIGNMENT);

		m_block		 = new_block;
		m_block_size = layout.total_size;
		m_hot		 = new_hot;

		if constexpr (HAS_COLD)
			m_cold = new_cold;

		m_slots		= new_slots;
		m_free_head = new_free_head;
		m_capacity	= new_capacity;

		EMBER_ASSERT(new_free_count == m_capacity - m_size);
	}

	template <class Tag, class Hot, class Cold> void Pool<Tag, Hot, Cold>::take_storage(Pool& other) noexcept
	{
		m_block		 = std::exchange(other.m_block, nullptr);
		m_block_size = std::exchange(other.m_block_size, 0);
		m_hot		 = std::exchange(other.m_hot, nullptr);
		m_cold		 = other.m_cold;
		other.m_cold = {};
		m_slots		 = std::exchange(other.m_slots, nullptr);
		m_free_head	 = std::exchange(other.m_free_head, INVALID_INDEX);
		m_size		 = std::exchange(other.m_size, 0);
		m_capacity	 = std::exchange(other.m_capacity, 0);
	}

	template <class Tag, class Hot, class Cold> void Pool<Tag, Hot, Cold>::release() noexcept
	{
		if constexpr (!std::is_trivially_destructible_v<Hot> || !std::is_trivially_destructible_v<ColdType>)
			retire_all();

		if (m_block != nullptr)
			m_resource->deallocate(m_block, m_block_size, BLOCK_ALIGNMENT);

		reset_storage();
	}

	template <class Tag, class Hot, class Cold> void Pool<Tag, Hot, Cold>::reset_storage() noexcept
	{
		m_block		 = nullptr;
		m_block_size = 0;
		m_hot		 = nullptr;
		m_cold		 = {};
		m_slots		 = nullptr;
		m_free_head	 = INVALID_INDEX;
		m_size		 = 0;
		m_capacity	 = 0;
	}
}
