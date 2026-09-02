#pragma once

#include <ember/core/bits.h>
#include <ember/core/common.h>
#include <ember/core/handle.h>
#include <ember/memory/memory.h>

#include <algorithm>

#pragma once

#include <ember/core/bits.h>
#include <ember/core/common.h>
#include <ember/core/handle.h>
#include <ember/memory/memory.h>

#include <algorithm>

namespace ember
{
	/**
	 * Fixed-capacity generational storage for persistent engine resources.
	 *
	 * A sparse slot map with weak-reference semantics: a handle's index addresses
	 * its slot directly, slots never move, and pointers stay valid until the pool
	 * is destroyed. The GPU layer relies on the direct addressing because a Handle's
	 * index is also its bindless heap slot.
	 *
	 * ComponentT sets the handle's index and generation width. The u16 default keeps
	 * handles at 32 bits and covers the GPU resource pools; pools indexing GPU tables
	 * past 65536 slots (renderer objects) widen to u32 and carry 64 bit handles.
	 * Only the per slot metadata grows with the width.
	 *
	 * Each slot carries a generation. Freeing the slot advances it, skipping zero,
	 * and create mints the slot's current generation, so get() is one compare and
	 * stale or null handles return nullptr. A slot's handles repeat once the
	 * generation counter wraps; treat the generation space as a safety budget.
	 *
	 * Free indices live in a FIFO ring: create pops the oldest free slot, so reuse
	 * cycles through every free slot before revisiting one and generation churn spreads
	 * evenly across the pool.
	 *
	 * erase() frees a slot in one step. When an index stays visible to work that
	 * outlives the handle (GPU frames in flight), retire() ends the handle now and
	 * release_slot() returns the storage later; size() counts live and retired alike.
	 *
	 * The live bits serve iteration, erase and debug checks. get() never reads them.
	 *
	 * init() sizes the pool once. A full pool returns a null handle from insert and
	 * emplace. A pool before init() behaves the same, since its capacity is zero.
	 *
	 * The pool is neither thread-safe nor re-entrant. Its memory resource must outlive it.
	 */
	template <class Tag, class HotT = Tag, class ColdT = void, class Component = u16> class Pool
	{
		struct NoColdStorage
		{
		};

	public:
		static constexpr bool HAS_COLD_STORAGE = !std::is_void_v<ColdT>;

		/// Every component index is addressable; the u32 bookkeeping bounds what a
		/// u32 component could otherwise name.
		static constexpr u64 INDEX_SPACE  = u64{1} << std::numeric_limits<Component>::digits;
		static constexpr u32 MAX_CAPACITY = static_cast<u32>(std::min<u64>(INDEX_SPACE, 0xFFFF'FFFF));

		using Hot		 = HotT;
		using Cold		 = std::conditional_t<HAS_COLD_STORAGE, ColdT, NoColdStorage>;
		using HandleType = Handle<Tag, Component>;

		template <bool IsConst> class SlotIterator;

		using Iterator		= SlotIterator<false>;
		using ConstIterator = SlotIterator<true>;

		static_assert(
			std::unsigned_integral<Component> && !std::same_as<Component, bool>,
			"Pool handles use an unsigned integral component");

		static_assert(
			std::is_nothrow_destructible_v<Hot> && std::is_nothrow_destructible_v<Cold>,
			"Pool values must be nothrow-destructible");

		explicit Pool(MemoryTag tag = MemoryTag::Unknown) noexcept;
		Pool(std::pmr::memory_resource& resource, MemoryTag tag = MemoryTag::Unknown) noexcept;
		~Pool() noexcept;

		// No copy
		Pool(const Pool&)			 = delete;
		Pool& operator=(const Pool&) = delete;

		Pool(Pool&& other) noexcept;
		Pool& operator=(Pool&& other) noexcept;

		/// Allocates capacity slots, in [1, MAX_CAPACITY]. Runs once, before any insert.
		/// A fresh pool hands out indices in ascending order from zero.
		void init(u32 capacity) noexcept;

		/// Constructs the HotType in-place in the next available slot
		template <class... Args> [[nodiscard]] EMBER_FINLINE HandleType emplace(Args&&... args) noexcept;

		/// Inserts (copies) the HotType in the next available slot
		[[nodiscard]] EMBER_FINLINE HandleType insert(const Hot& value) noexcept;

		/// Inserts (moves) the HotType in the next available slot
		[[nodiscard]] EMBER_FINLINE HandleType insert(Hot&& value) noexcept;

		template <class HotArg, class ColdArg>
		[[nodiscard]] EMBER_FINLINE HandleType insert(HotArg&& hot, ColdArg&& cold) noexcept
			requires(HAS_COLD_STORAGE);

		[[nodiscard]] EMBER_FINLINE bool erase(HandleType handle) noexcept;

		/// Retires the slot: the handle dies and the values are destroyed now, but the
		/// slot stays claimed until release_slot(). For resources whose index stays
		/// visible to in-flight GPU work, retire at destroy and release at the fence.
		[[nodiscard]] EMBER_FINLINE bool retire(HandleType handle) noexcept;

		/// Returns a retired slot to the free ring. The caller proves the slot is no
		/// longer referenced; nothing here can check that.
		EMBER_FINLINE void release_slot(Component index) noexcept;

		[[nodiscard]] EMBER_FINLINE bool contains(HandleType handle) const noexcept;

		/// Weak deref: nullptr when the handle is stale or null.
		[[nodiscard]] EMBER_FINLINE Hot* get(HandleType handle) noexcept;
		[[nodiscard]] EMBER_FINLINE const Hot* get(HandleType handle) const noexcept;

		/// Weak deref: nullptr when the handle is stale or null.
		[[nodiscard]] EMBER_FINLINE Cold* get_cold(HandleType handle) noexcept
			requires(HAS_COLD_STORAGE);
		[[nodiscard]] EMBER_FINLINE const Cold* get_cold(HandleType handle) const noexcept
			requires(HAS_COLD_STORAGE);

		/// Occupied slots, live plus retired.
		[[nodiscard]] EMBER_FINLINE u32 size() const noexcept;

		/// Retired slots awaiting release_slot().
		[[nodiscard]] EMBER_FINLINE u32 retired_count() const noexcept;

		[[nodiscard]] EMBER_FINLINE u32 capacity() const noexcept;
		[[nodiscard]] EMBER_FINLINE bool empty() const noexcept;

		void clear() noexcept;

		[[nodiscard]] EMBER_FINLINE Iterator begin() noexcept;
		[[nodiscard]] EMBER_FINLINE Iterator end() noexcept;
		[[nodiscard]] EMBER_FINLINE ConstIterator begin() const noexcept;
		[[nodiscard]] EMBER_FINLINE ConstIterator end() const noexcept;

	private:
		static constexpr u32 INVALID_INDEX		= std::numeric_limits<u32>::max();
		static constexpr size_t VALUE_ALIGNMENT = std::max(alignof(Hot), alignof(Cold));
		static constexpr size_t BLOCK_ALIGNMENT = std::max(VALUE_ALIGNMENT, static_cast<size_t>(EMBER_CACHE_LINE));

		/**
		 * Physical allocation:
		 * [Hot][Cold?][generations][free-index ring][live bits]
		 */
		struct Layout
		{
			size_t cold_offset = 0;
			size_t gens_offset = 0;
			size_t free_offset = 0;
			size_t live_offset = 0;
			size_t total_size  = 0;
		};

		[[nodiscard]] EMBER_FINLINE bool is_live(u32 index) const noexcept;
		EMBER_FINLINE void set_live(u32 index) noexcept;
		EMBER_FINLINE void clear_live(u32 index) noexcept;

		/// Pops the ring head. INVALID_INDEX when the pool is full or init() has not run.
		/// The slot stays dead until publish_slot.
		[[nodiscard]] EMBER_FINLINE u32 acquire_slot() noexcept;
		[[nodiscard]] EMBER_FINLINE HandleType publish_slot(u32 index) noexcept;
		[[nodiscard]] EMBER_FINLINE HandleType make_handle(u32 index) const noexcept;

		EMBER_FINLINE void free_push(Component index) noexcept;

		/// Housekeeping.
		void destroy_slot(u32 index) noexcept;
		void destroy_all() noexcept;
		void reset_free_ring() noexcept;

		[[noreturn]] void fail_allocation(size_t requested_size) const noexcept;
		[[nodiscard]] size_t carve(size_t& cursor, size_t count, size_t stride, size_t align) const noexcept;
		[[nodiscard]] Layout make_layout(u32 capacity) const noexcept;

		void take_storage(Pool& other) noexcept;
		void release() noexcept;
		void reset_storage() noexcept;

		// Non-owning pointer to the PMR resource that backs this Pool
		std::pmr::memory_resource* m_resource = nullptr;
		MemoryTag m_tag						  = MemoryTag::Unknown;

		void* m_block		= nullptr;
		size_t m_block_size = 0;

		Hot* m_hot						   = nullptr;
		[[no_unique_address]] Cold* m_cold = nullptr;

		Component* m_generations = nullptr;
		Component* m_free		 = nullptr;
		u64* m_live				 = nullptr;
		u32 m_free_head			 = 0;
		u32 m_free_count		 = 0;
		u32 m_retired			 = 0;
		u32 m_capacity			 = 0;
	};
}

#include <ember/containers/pool.inl>
