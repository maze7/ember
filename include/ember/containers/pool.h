#pragma once

#include <ember/core/bits.h>
#include <ember/core/common.h>
#include <ember/core/handle.h>
#include <ember/memory/memory.h>

namespace ember
{
	/**
	 * Sparse generational storage for persistent engine resources.
	 *
	 * Handles map directly to stable slots. Erasing one resource does not move
	 * another. reserve() may relocate live objects, invalidating pointers and
	 * iterators while preserving handles.
	 *
	 * Reserve pools during initialization to prevent allocations and relocation
	 * during frame execution.
	 *
	 * The pool is neither thread-safe nor re-entrant. Its memory resource must
	 * outlive it.
	 */
	template <class Tag, class Hot = Tag, class Cold = void> class Pool
	{
		struct EmptyCold{};

	public:
		static constexpr bool HAS_COLD = !std::is_void_v<Cold>;

		using HandleType = Handle<Tag, u16>;
		using HotType	 = Hot;
		using ColdType	 = std::conditional_t<HAS_COLD, Cold, EmptyCold>;

		// Forward declaration of iterator
		template <bool IsConst> class SlotIterator;

		using Iterator		= SlotIterator<false>;
		using ConstIterator = SlotIterator<true>;

		explicit Pool(MemoryTag tag = MemoryTag::Unknown) noexcept;
		Pool(std::pmr::memory_resource& resource, MemoryTag diagnostics_tag = MemoryTag::Unknown) noexcept;
		~Pool() noexcept;

		// No copy
		Pool(const Pool&)			 = delete;
		Pool& operator=(const Pool&) = delete;

		Pool(Pool&& other) noexcept;
		Pool& operator=(Pool&& other) noexcept;

		template <class... Args> [[nodiscard]] EMBER_FINLINE HandleType emplace(Args&&... args) noexcept;

		[[nodiscard]] EMBER_FINLINE HandleType insert(const Hot& value) noexcept;
		[[nodiscard]] EMBER_FINLINE HandleType insert(Hot&& value) noexcept;

		template <class HotArg, class ColdArg>
		[[nodiscard]] EMBER_FINLINE HandleType insert(HotArg&& hot, ColdArg&& cold) noexcept
			requires(HAS_COLD);

		[[nodiscard]] EMBER_FINLINE bool erase(HandleType handle) noexcept;
		[[nodiscard]] EMBER_FINLINE bool contains(HandleType handle) const noexcept;

		[[nodiscard]] EMBER_FINLINE Hot* try_get(HandleType handle) noexcept;
		[[nodiscard]] EMBER_FINLINE const Hot* try_get(HandleType handle) const noexcept;

		[[nodiscard]] EMBER_FINLINE Hot& get(HandleType handle) noexcept;
		[[nodiscard]] EMBER_FINLINE const Hot& get(HandleType handle) const noexcept;

		[[nodiscard]] EMBER_FINLINE ColdType* try_get_cold(HandleType handle) noexcept
			requires(HAS_COLD);

		[[nodiscard]] EMBER_FINLINE const ColdType* try_get_cold(HandleType handle) const noexcept
			requires(HAS_COLD);

		[[nodiscard]] EMBER_FINLINE ColdType& get_cold(HandleType handle) noexcept
			requires(HAS_COLD);

		[[nodiscard]] EMBER_FINLINE const ColdType& get_cold(HandleType handle) const noexcept
			requires(HAS_COLD);

		[[nodiscard]] EMBER_FINLINE u32 size() const noexcept;
		[[nodiscard]] EMBER_FINLINE u32 capacity() const noexcept;
		[[nodiscard]] EMBER_FINLINE bool empty() const noexcept;

		void reserve(u32 requested_capacity) noexcept;
		void clear() noexcept;

		[[nodiscard]] EMBER_FINLINE Iterator begin() noexcept;
		[[nodiscard]] EMBER_FINLINE Iterator end() noexcept;
		[[nodiscard]] EMBER_FINLINE ConstIterator begin() const noexcept;
		[[nodiscard]] EMBER_FINLINE ConstIterator end() const noexcept;

	private:
		static constexpr u16 OCCUPIED	   = std::numeric_limits<u16>::max();
		static constexpr u32 INVALID_INDEX = std::numeric_limits<u32>::max();
		static constexpr u32 MAX_CAPACITY  = std::numeric_limits<u16>::max();
		static constexpr u32 MIN_CAPACITY  = 16;

		static constexpr size_t VALUE_ALIGNMENT =
			alignof(HotType) > alignof(ColdType)
				? alignof(HotType)
				: alignof(ColdType);

		static constexpr size_t BLOCK_ALIGNMENT =
			VALUE_ALIGNMENT > EMBER_CACHE_LINE
				? VALUE_ALIGNMENT
				: EMBER_CACHE_LINE;

		/**
		 * Combined handle metadata and intrusive free-list node.
		 *
		 * state == OCCUPIED: the slot contains live Hot/Cold objects.
		 * state != OCCUPIED: state is the next free index when another free
		 * 	slot follows. The final free slot's state is unused.
		 */
		struct alignas(u32) Slot
		{
			u16 generation = 1;
			u16 state  = 0;
		};

		/**
		 * Physical allocation:
		 * [Hot][Cold?][Slot metadata]
		 */
		struct Layout
		{
			size_t cold_offset	= 0;
			size_t slots_offset = 0;
			size_t total_size	= 0;
		};

		using ColdPointer = std::conditional_t<HAS_COLD, ColdType*, EmptyCold>;

		static_assert(
			std::is_nothrow_destructible_v<HotType> && std::is_nothrow_destructible_v<ColdType>,
			"Pool values must be nothrow-destructible");

		static_assert(
			(std::is_trivially_copyable_v<HotType> || std::is_nothrow_move_constructible_v<HotType>) &&
				(std::is_trivially_copyable_v<ColdType> || std::is_nothrow_move_constructible_v<ColdType>),
			"Pool values must be trivially copyable or nothrow move-constructible");

		static_assert(std::is_trivially_copyable_v<Slot>);

		template <class... Args>
		[[nodiscard]] EMBER_FINLINE HandleType emplace_no_grow(Args&&... args) noexcept;

		template <class... Args>
		[[nodiscard]] HandleType emplace_grow(Args&&... args) noexcept;

		template <class HotArg, class ColdArg>
		[[nodiscard]] EMBER_FINLINE HandleType insert_no_grow(HotArg&& hot, ColdArg&& cold) noexcept
			requires(HAS_COLD);

		template <class HotArg, class ColdArg>
		[[nodiscard]] HandleType insert_grow(HotArg&& hot, ColdArg&& cold) noexcept
			requires(HAS_COLD);

		[[nodiscard]] EMBER_FINLINE u32 acquire_slot() noexcept;
		[[nodiscard]] EMBER_FINLINE HandleType publish_slot(u32 index) noexcept;
		[[nodiscard]] EMBER_FINLINE HandleType make_handle(u32 index) const noexcept;

		static EMBER_FINLINE void advance_generation(Slot& slot) noexcept;

		void retire(u32 index) noexcept;
		void retire_all() noexcept;
		void rebuild_free_list() noexcept;

		[[noreturn]] void fail_allocation(size_t requested_size) const noexcept;

		[[nodiscard]] size_t carve(size_t& cursor, size_t count, size_t stride, size_t align) const noexcept;

		[[nodiscard]] Layout make_layout(u32 capacity) const noexcept;

		void relocate_values(Hot* new_hot, ColdType* new_cold) noexcept;
		void reallocate(u32 new_capacity) noexcept;

		void take_storage(Pool& other) noexcept;
		void release() noexcept;
		void reset_storage() noexcept;

		// Non-owning pointer to the PMR resource that backs this Pool
		std::pmr::memory_resource* m_resource = nullptr;
		MemoryTag m_tag = MemoryTag::Unknown;

		void* m_block = nullptr;
		size_t m_block_size = 0;

		Hot* m_hot = nullptr;

		[[no_unique_address]] ColdPointer m_cold{};

		Slot* m_slots = nullptr;
		u32 m_free_head = INVALID_INDEX;
		u32 m_size = 0;
		u32 m_capacity = 0;
	};
}

#include <ember/containers/pool.inl>
