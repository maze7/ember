#pragma once

#include "ember/containers/pool.h"

namespace ember
{
	template <class Tag, class Hot, class Cold, class Component>
	template <bool IsConst>
	class Pool<Tag, Hot, Cold, Component>::SlotIterator
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
			while (m_index < m_pool->m_capacity && !m_pool->is_live(m_index))
				++m_index;
		}

		PoolPointer m_pool = nullptr;
		u32 m_index		   = 0;
	};

	template <class Tag, class Hot, class Cold, class Component>
	Pool<Tag, Hot, Cold, Component>::Pool(MemoryTag tag) noexcept : Pool(memory::heap(tag), tag)
	{
	}

	template <class Tag, class Hot, class Cold, class Component>
	Pool<Tag, Hot, Cold, Component>::Pool(std::pmr::memory_resource& resource, MemoryTag diagnostics_tag) noexcept
		: m_resource(&resource), m_tag(diagnostics_tag)
	{
	}

	template <class Tag, class Hot, class Cold, class Component> Pool<Tag, Hot, Cold, Component>::~Pool() noexcept
	{
		release();
	}

	template <class Tag, class Hot, class Cold, class Component>
	Pool<Tag, Hot, Cold, Component>::Pool(Pool&& other) noexcept : m_resource(other.m_resource), m_tag(other.m_tag)
	{
		take_storage(other);
	}

	template <class Tag, class Hot, class Cold, class Component>
	auto Pool<Tag, Hot, Cold, Component>::operator=(Pool&& other) noexcept -> Pool&
	{
		if (this == &other)
			return *this;

		release();

		m_resource = other.m_resource;
		m_tag	   = other.m_tag;

		take_storage(other);
		return *this;
	}

	template <class Tag, class Hot, class Cold, class Component>
	void Pool<Tag, Hot, Cold, Component>::init(u32 capacity) noexcept
	{
		EMBER_ASSERT(m_block == nullptr && "init runs once");
		EMBER_ASSERT(capacity != 0 && capacity <= MAX_CAPACITY);

		const Layout layout = make_layout(capacity);

		m_block		 = m_resource->allocate(layout.total_size, BLOCK_ALIGNMENT);
		m_block_size = layout.total_size;

		auto* base = static_cast<std::byte*>(m_block);
		m_hot	   = reinterpret_cast<Hot*>(base);

		if constexpr (HAS_COLD_STORAGE)
			m_cold = reinterpret_cast<Cold*>(base + layout.cold_offset);

		m_generations = reinterpret_cast<Component*>(base + layout.gens_offset);
		m_free		  = reinterpret_cast<Component*>(base + layout.free_offset);
		m_live		  = reinterpret_cast<u64*>(base + layout.live_offset);
		m_capacity	  = capacity;

		// Generation zero is reserved for the null handle.
		for (u32 index = 0; index < capacity; ++index)
			m_generations[index] = 1;

		std::memset(m_live, 0, ((capacity + 63) / 64) * sizeof(u64));

		reset_free_ring();
	}

	template <class Tag, class Hot, class Cold, class Component>
	template <class... Args>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::emplace(Args&&... args) noexcept -> HandleType
	{
		const u32 index = acquire_slot();

		if (index == INVALID_INDEX) [[unlikely]]
			return {};

		std::construct_at(m_hot + index, std::forward<Args>(args)...);

		if constexpr (HAS_COLD_STORAGE)
			std::construct_at(m_cold + index);

		return publish_slot(index);
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::insert(Hot&& value) noexcept -> HandleType
	{
		return emplace(std::move(value));
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::insert(const Hot& value) noexcept -> HandleType
	{
		return emplace(value);
	}

	template <class Tag, class Hot, class Cold, class Component>
	template <class HotArg, class ColdArg>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::insert(HotArg&& hot, ColdArg&& cold) noexcept -> HandleType
		requires(HAS_COLD_STORAGE)
	{
		const u32 index = acquire_slot();

		if (index == INVALID_INDEX) [[unlikely]]
			return {};

		std::construct_at(m_hot + index, std::forward<HotArg>(hot));
		std::construct_at(m_cold + index, std::forward<ColdArg>(cold));

		return publish_slot(index);
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE bool Pool<Tag, Hot, Cold, Component>::erase(HandleType handle) noexcept
	{
		if (!retire(handle))
			return false;

		release_slot(handle.index);
		return true;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE bool Pool<Tag, Hot, Cold, Component>::retire(HandleType handle) noexcept
	{
		if (handle.index >= m_capacity) [[unlikely]]
			return false;

		const u32 index = static_cast<u32>(handle.index);

		// The live test keeps a generation-matching garbage handle from retiring
		// a slot twice and double-counting it.
		if (m_generations[index] != handle.generation || !is_live(index))
			return false;

		destroy_slot(index);
		++m_retired;

		return true;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE void Pool<Tag, Hot, Cold, Component>::release_slot(Component index) noexcept
	{
		EMBER_ASSERT(index < m_capacity);
		EMBER_ASSERT(!is_live(index) && "release of a live slot");
		EMBER_ASSERT(m_retired != 0 && "release without a matching retire");

		--m_retired;
		free_push(index);
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE bool Pool<Tag, Hot, Cold, Component>::contains(HandleType handle) const noexcept
	{
		if (handle.index >= m_capacity) [[unlikely]]
			return false;

		return m_generations[handle.index] == handle.generation && is_live(handle.index);
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::get(HandleType handle) noexcept -> Hot*
	{
		if (handle.index >= m_capacity) [[unlikely]]
			return nullptr;

		const bool match = m_generations[handle.index] == handle.generation;

		EMBER_ASSERT(!match || is_live(handle.index));

		return match ? m_hot + handle.index : nullptr;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::get(HandleType handle) const noexcept -> const Hot*
	{
		if (handle.index >= m_capacity) [[unlikely]]
			return nullptr;

		const bool match = m_generations[handle.index] == handle.generation;

		EMBER_ASSERT(!match || is_live(handle.index));

		return match ? m_hot + handle.index : nullptr;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::get_cold(HandleType handle) noexcept -> Cold*
		requires(HAS_COLD_STORAGE)
	{
		if (handle.index >= m_capacity) [[unlikely]]
			return nullptr;

		const bool match = m_generations[handle.index] == handle.generation;

		EMBER_ASSERT(!match || is_live(handle.index));

		return match ? m_cold + handle.index : nullptr;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::get_cold(HandleType handle) const noexcept -> const Cold*
		requires(HAS_COLD_STORAGE)
	{
		if (handle.index >= m_capacity) [[unlikely]]
			return nullptr;

		const bool match = m_generations[handle.index] == handle.generation;

		EMBER_ASSERT(!match || is_live(handle.index));

		return match ? m_cold + handle.index : nullptr;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE u32 Pool<Tag, Hot, Cold, Component>::size() const noexcept
	{
		return m_capacity - m_free_count;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE u32 Pool<Tag, Hot, Cold, Component>::retired_count() const noexcept
	{
		return m_retired;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE u32 Pool<Tag, Hot, Cold, Component>::capacity() const noexcept
	{
		return m_capacity;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE bool Pool<Tag, Hot, Cold, Component>::empty() const noexcept
	{
		return m_free_count == m_capacity;
	}

	template <class Tag, class Hot, class Cold, class Component> void Pool<Tag, Hot, Cold, Component>::clear() noexcept
	{
		destroy_all();
		reset_free_ring();
		m_retired = 0;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::begin() noexcept -> Iterator
	{
		return Iterator(this, 0);
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::end() noexcept -> Iterator
	{
		return Iterator(this, m_capacity);
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::begin() const noexcept -> ConstIterator
	{
		return ConstIterator(this, 0);
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::end() const noexcept -> ConstIterator
	{
		return ConstIterator(this, m_capacity);
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE bool Pool<Tag, Hot, Cold, Component>::is_live(u32 index) const noexcept
	{
		return (m_live[index >> 6] & (u64{1} << (index & 63))) != 0;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE void Pool<Tag, Hot, Cold, Component>::set_live(u32 index) noexcept
	{
		m_live[index >> 6] |= u64{1} << (index & 63);
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE void Pool<Tag, Hot, Cold, Component>::clear_live(u32 index) noexcept
	{
		m_live[index >> 6] &= ~(u64{1} << (index & 63));
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE u32 Pool<Tag, Hot, Cold, Component>::acquire_slot() noexcept
	{
		if (m_free_count == 0) [[unlikely]]
			return INVALID_INDEX;

		const u32 index = m_free[m_free_head];

		EMBER_ASSERT(index < m_capacity);
		EMBER_ASSERT(!is_live(index));

		m_free_head = m_free_head + 1 == m_capacity ? 0 : m_free_head + 1;
		--m_free_count;

		return index;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE void Pool<Tag, Hot, Cold, Component>::free_push(Component index) noexcept
	{
		EMBER_ASSERT(m_free_count < m_capacity);

		u32 tail = m_free_head + m_free_count;
		if (tail >= m_capacity)
			tail -= m_capacity;

		m_free[tail] = index;
		++m_free_count;
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::publish_slot(u32 index) noexcept -> HandleType
	{
		EMBER_ASSERT(index < m_capacity);
		EMBER_ASSERT(!is_live(index));
		EMBER_ASSERT(m_generations[index] != 0);

		set_live(index);

		return {static_cast<Component>(index), m_generations[index]};
	}

	template <class Tag, class Hot, class Cold, class Component>
	EMBER_FINLINE auto Pool<Tag, Hot, Cold, Component>::make_handle(u32 index) const noexcept -> HandleType
	{
		EMBER_ASSERT(index < m_capacity);
		EMBER_ASSERT(is_live(index));

		return {static_cast<Component>(index), m_generations[index]};
	}

	template <class Tag, class Hot, class Cold, class Component>
	void Pool<Tag, Hot, Cold, Component>::destroy_slot(u32 index) noexcept
	{
		EMBER_ASSERT(index < m_capacity);
		EMBER_ASSERT(is_live(index));

		// Invalidate before user destructors run. Zero is skipped so the null
		// handle stays unmatchable.
		Component generation = static_cast<Component>(m_generations[index] + 1);
		if (generation == 0) [[unlikely]]
			generation = 1;

		m_generations[index] = generation;
		clear_live(index);

		if constexpr (HAS_COLD_STORAGE)
			std::destroy_at(m_cold + index);
		std::destroy_at(m_hot + index);
	}

	template <class Tag, class Hot, class Cold, class Component>
	void Pool<Tag, Hot, Cold, Component>::destroy_all() noexcept
	{
		for (u32 index = 0; index < m_capacity; ++index)
		{
			if (is_live(index))
				destroy_slot(index);
		}
	}

	template <class Tag, class Hot, class Cold, class Component>
	void Pool<Tag, Hot, Cold, Component>::reset_free_ring() noexcept
	{
		for (u32 index = 0; index < m_capacity; ++index)
			m_free[index] = static_cast<Component>(index);

		m_free_head	 = 0;
		m_free_count = m_capacity;
	}

	template <class Tag, class Hot, class Cold, class Component>
	[[noreturn]] void Pool<Tag, Hot, Cold, Component>::fail_allocation(size_t requested_size) const noexcept
	{
		out_of_memory(requested_size, BLOCK_ALIGNMENT, m_tag);
	}

	template <class Tag, class Hot, class Cold, class Component>
	size_t
	Pool<Tag, Hot, Cold, Component>::carve(size_t& cursor, size_t count, size_t stride, size_t alignment) const noexcept
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

	template <class Tag, class Hot, class Cold, class Component>
	auto Pool<Tag, Hot, Cold, Component>::make_layout(u32 capacity) const noexcept -> Layout
	{
		const size_t count = capacity;
		size_t cursor	   = 0;
		Layout layout{};

		// The allocation itself aligns the beginning of the hot stream.
		(void)carve(cursor, count, sizeof(Hot), BLOCK_ALIGNMENT);

		if constexpr (HAS_COLD_STORAGE)
			layout.cold_offset = carve(cursor, count, sizeof(Cold), BLOCK_ALIGNMENT);

		// Metadata starts on a separate cache-line boundary.
		layout.gens_offset = carve(cursor, count, sizeof(Component), BLOCK_ALIGNMENT);
		layout.free_offset = carve(cursor, count, sizeof(Component), BLOCK_ALIGNMENT);
		layout.live_offset = carve(cursor, (count + 63) / 64, sizeof(u64), BLOCK_ALIGNMENT);
		layout.total_size  = cursor;
		return layout;
	}

	template <class Tag, class Hot, class Cold, class Component>
	void Pool<Tag, Hot, Cold, Component>::take_storage(Pool& other) noexcept
	{
		m_block		 = std::exchange(other.m_block, nullptr);
		m_block_size = std::exchange(other.m_block_size, 0);
		m_hot		 = std::exchange(other.m_hot, nullptr);
		m_cold		 = other.m_cold;
		other.m_cold = {};

		m_generations = std::exchange(other.m_generations, nullptr);
		m_free		  = std::exchange(other.m_free, nullptr);
		m_live		  = std::exchange(other.m_live, nullptr);
		m_free_head	  = std::exchange(other.m_free_head, 0);
		m_free_count  = std::exchange(other.m_free_count, 0);
		m_retired	  = std::exchange(other.m_retired, 0);
		m_capacity	  = std::exchange(other.m_capacity, 0);
	}

	template <class Tag, class Hot, class Cold, class Component>
	void Pool<Tag, Hot, Cold, Component>::release() noexcept
	{
		if constexpr (!std::is_trivially_destructible_v<Hot> || !std::is_trivially_destructible_v<Cold>)
			destroy_all();

		if (m_block != nullptr)
			m_resource->deallocate(m_block, m_block_size, BLOCK_ALIGNMENT);

		reset_storage();
	}

	template <class Tag, class Hot, class Cold, class Component>
	void Pool<Tag, Hot, Cold, Component>::reset_storage() noexcept
	{
		m_block		  = nullptr;
		m_block_size  = 0;
		m_hot		  = nullptr;
		m_cold		  = {};
		m_generations = nullptr;
		m_free		  = nullptr;
		m_live		  = nullptr;
		m_free_head	  = 0;
		m_free_count  = 0;
		m_retired	  = 0;
		m_capacity	  = 0;
	}
}
