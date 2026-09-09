#pragma once

#include <ember/core/bits.h>
#include <ember/memory/tagged_heap.h>

#include <memory_resource>

namespace ember
{
	/**
	 * A linear allocator over blocks from a TaggedHeap, following the block allocators in Christian
	 * Gyrling's GDC 2015 talk. Everything it hands out belongs to its tag and dies together when
	 * the tag is released; there is no way to free one allocation.
	 *
	 * Each frame thread bumps a pointer inside a block of its own, so allocation costs no atomics
	 * and no two threads ever write the same cache line. Taking the next block is a few atomics in
	 * the heap, once per block rather than once per allocation.
	 *
	 * All of them share this one object because a PMR container stores the resource pointer it was
	 * built with: a job that fills a vector, waits, and resumes on another worker has to keep
	 * allocating through the same resource. It then bumps a different thread's block, under the
	 * same tag, which is exactly what the talk describes.
	 *
	 * Frame threads claim a slot with register_thread() before they allocate: main and the job
	 * system's workers. Any other thread asserts. A detached job must not allocate here unless it
	 * is certain to finish before the tag is released.
	 */
	class BlockAllocator final : public std::pmr::memory_resource
	{
	public:
		/// Threads that can hold a slot at once. One bit each in the slot mask.
		static constexpr u32 MAX_THREADS = 64;
		static constexpr u32 NO_SLOT	 = ~u32{0};

		constexpr BlockAllocator() noexcept = default;
		~BlockAllocator() noexcept override;

		BlockAllocator(const BlockAllocator&)			 = delete;
		BlockAllocator& operator=(const BlockAllocator&) = delete;

		/// Points the allocator at a heap and names the memory it will hand out.
		void init(TaggedHeap& heap, HeapTag tag) noexcept;

		/// Releases the tag and detaches from the heap.
		void shutdown() noexcept;

		/**
		 * Claims a slot for the calling thread, once, before its first allocation. The slot belongs
		 * to the thread rather than to one allocator, so a single call covers every BlockAllocator.
		 * Claim on the thread's way in and release on its way out, never mid frame.
		 */
		static void register_thread() noexcept;
		static void unregister_thread() noexcept;

		/// The calling thread's slot, NO_SLOT if it never claimed one. Never inlined: a job resumes
		/// on whichever worker took it, so each allocation has to ask again.
		[[nodiscard]] static EMBER_NOINLINE u32 thread_slot() noexcept;

		// Bump allocation fast path: public and non-virtual so hot code holding the concrete type
		// inlines it, the way ArenaResource does. Zero sized allocations take a byte, so every
		// pointer handed out is unique until the tag is released.
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

		/// Gives every block back to the heap. The tag's sync point: no job may still hold memory
		/// from it, and no pointer into it may be read afterwards.
		void reset() noexcept;

		/// Renames the memory this allocator hands out from here on, for per frame tags. The old
		/// tag's blocks are left alone, so whoever owns that frame releases them.
		void retag(HeapTag tag) noexcept;

		[[nodiscard]] HeapTag tag() const noexcept { return m_tag; }

		// True if ptr lies inside the heap's reservation, whichever tag owns that block.
		[[nodiscard]] bool owns(const void* ptr) const noexcept { return m_heap != nullptr && m_heap->owns(ptr); }

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
			(void)ptr; // Reclaimed wholesale when the tag is released.
		}

		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

		[[nodiscard]] Slot& current_slot() noexcept
		{
			const u32 slot = thread_slot();
			EMBER_ASSERT(slot < MAX_THREADS && "block memory belongs to registered frame threads");
			return m_slots[slot];
		}

		[[nodiscard]] void* allocate_from_blocks(Slot& slot, size_t bytes, size_t alignment) noexcept;

		void clear_slots() noexcept;

		Slot		m_slots[MAX_THREADS];
		TaggedHeap* m_heap = nullptr;
		HeapTag		m_tag  = NO_TAG;
	};
}
