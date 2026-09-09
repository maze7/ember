#pragma once

#include <ember/core/bits.h>
#include <ember/memory/common.h>

#include <atomic>
#include <memory_resource>

namespace ember
{
	/**
	 * Per frame CPU scratch that every frame thread allocates from at once. One reservation is cut
	 * into fixed blocks; a thread bumps a pointer inside the block it holds and takes the next free
	 * blocks when that one runs out. Nothing is released during the frame, and reset() at the frame
	 * boundary hands every block back at the same time.
	 *
	 * All of them share this one object because a PMR container stores the resource pointer it was
	 * built with: a job that fills a vector, waits, and resumes on another worker has to keep
	 * allocating through the same resource. The per thread state lives inside, reached through the
	 * calling thread's slot.
	 *
	 * Frame threads claim a slot with register_thread() before they allocate: main and the job
	 * system's workers. Any other thread asserts, and a detached job must not use frame memory at
	 * all, because it can outlive the reset that takes its memory away.
	 *
	 * Blocks are handed out and never returned until the reset, so the free list is one counter.
	 * An allocation larger than a block takes as many neighbouring blocks as it needs. Running out
	 * of them is out of memory: this is a budget, sized for the game like every other engine pool.
	 */
	class FrameResource final : public std::pmr::memory_resource
	{
	public:
		/// Frame threads that can hold a slot at once. One bit each in the slot mask.
		static constexpr u32 MAX_THREADS = 64;
		static constexpr u32 NO_SLOT	 = ~u32{0};

		constexpr FrameResource() noexcept = default;
		~FrameResource() noexcept override;

		FrameResource(const FrameResource&)			   = delete;
		FrameResource& operator=(const FrameResource&) = delete;

		/// block_size must be a power of two; capacity rounds up to a whole number of blocks and is
		/// committed here, so a block refill never becomes a system call.
		[[nodiscard]] bool init(size_t capacity, size_t block_size, MemoryTag tag = MemoryTag::Engine) noexcept;
		void shutdown() noexcept;

		/**
		 * Claims a slot for the calling thread, once, before its first allocation. The slot belongs
		 * to the thread rather than to one resource, so a single call covers every FrameResource.
		 * Claim on the thread's way in and release on its way out, never mid frame.
		 */
		static void register_thread() noexcept;
		static void unregister_thread() noexcept;

		/// The calling thread's slot, NO_SLOT if it never claimed one. Never inlined: a job resumes
		/// on whichever worker took it, so each allocation has to ask again.
		[[nodiscard]] static EMBER_NOINLINE u32 thread_slot() noexcept;

		// Bump allocation fast path: public and non-virtual so hot code holding the concrete type
		// inlines it, the way ArenaResource does. Zero sized allocations take a byte, so every
		// pointer handed out is unique until the reset.
		[[nodiscard]] void* allocate_fast(size_t size, size_t alignment = DEFAULT_ALIGNMENT) noexcept
		{
			EMBER_ASSERT(is_power_of_two(alignment));

			Slot& slot		   = current_slot();
			const size_t bytes = size == 0 ? 1 : size;
			u8* aligned		   = align_up(slot.cursor, alignment);

			// Aligning can walk off the end of the block, so the room left is only meaningful once
			// the cursor is known to be inside it. An empty slot has both pointers null and takes
			// the same path, which is how a thread's first allocation gets its first block.
			if (aligned >= slot.end || static_cast<size_t>(slot.end - aligned) < bytes) [[unlikely]]
				return allocate_from_blocks(slot, bytes, alignment);

			slot.cursor = aligned + bytes;
			return aligned;
		}

		/// Hands every block back. Frame boundary only, with no job left that holds frame memory.
		void reset() noexcept;

		// True if ptr lies inside the reservation. Used to catch a block from another resource
		// being freed through this one.
		[[nodiscard]] bool owns(const void* ptr) const noexcept
		{
			const u8* p = static_cast<const u8*>(ptr);
			return p >= m_base && p < m_base + m_capacity;
		}

		[[nodiscard]] size_t block_size() const noexcept { return m_block_size; }
		[[nodiscard]] size_t capacity() const noexcept { return m_capacity; }

		/// Bytes in the blocks handed out this frame, unused tails included.
		[[nodiscard]] size_t used() const noexcept
		{
			return m_next_block.load(std::memory_order_relaxed) * m_block_size;
		}

		/// The high water mark of used(), updated once per reset.
		[[nodiscard]] size_t peak() const noexcept { return m_peak_blocks * m_block_size; }

	private:
		// A cache line each: threads bump their cursors constantly and must not share a line.
		struct alignas(EMBER_CACHE_LINE) Slot
		{
			u8* cursor = nullptr;
			u8* end	   = nullptr;
		};

		void* do_allocate(size_t bytes, size_t alignment) noexcept override { return allocate_fast(bytes, alignment); }

		void do_deallocate(void* ptr, size_t /*bytes*/, size_t /*alignment*/) noexcept override
		{
			EMBER_ASSERT(ptr == nullptr || owns(ptr));
			(void)ptr; // Reclaimed wholesale by reset().
		}

		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

		[[nodiscard]] Slot& current_slot() noexcept
		{
			const u32 slot = thread_slot();
			EMBER_ASSERT(slot < MAX_THREADS && "frame memory belongs to registered frame threads");
			return m_slots[slot];
		}

		[[nodiscard]] void* allocate_from_blocks(Slot& slot, size_t bytes, size_t alignment) noexcept;

		Slot			 m_slots[MAX_THREADS];
		u8*				 m_base		   = nullptr;
		size_t			 m_capacity	   = 0;
		size_t			 m_block_size  = 0;
		u32				 m_block_count = 0;
		u32				 m_peak_blocks = 0;
		std::atomic<u32> m_next_block  = 0;
		MemoryTag		 m_tag		   = MemoryTag::Unknown;
	};
}
