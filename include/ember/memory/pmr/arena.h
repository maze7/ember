#pragma once

#include <ember/core/bits.h>
#include <ember/memory/tagged_heap.h>

#include <memory_resource>

namespace ember
{
	class Arena final : public std::pmr::memory_resource
	{
	public:
		// Threads that can hold a slot at once. One bit ecah in the slot mask.
		static constexpr u32 MAX_THREADS = 64;
		static constexpr u32 NO_SLOT	 = ~u32{0};

		/** What the arena has done since begin(). Sums every thread's slot, so read after end(). */
		struct Stats
		{
			size_t bytes_requested = 0; // what callers asked for
			size_t bytes_owned	   = 0; // blocks * block size; the difference is padding and tails
			u32 blocks			   = 0;
			u32 refills			   = 0; // heap allocations, oversized runs included
			u32 threads			   = 0; // slots that allocated at least once
		};

		Arena() noexcept = default;
		~Arena() noexcept override;

		/** Binds the arena to the heap it takes blocks from. Inactive until the first begin(). */
		void init(TaggedHeap& heap, const char* name = "arena") noexcept;

		/**
		 * Frees the current tag, whether or not end() was called, and detaches.
		 * Boot and teardown only; every frame goes through begin(), end() and the heap.
		 */
		void shutdown() noexcept;

		/**
		 * Starts handing out memory under the tag. The previous tag's blocks are left alone;
		 * whoever consumes them frees them through the heap.
		 */
		void begin(HeapTag tag) noexcept;

		/** Stops handing out memory and returns the tag, for the heap to free now or later. */
		HeapTag end() noexcept;

		bool is_active() const noexcept { return m_active; }
		HeapTag tag() const noexcept { return m_tag; }
		const char* name() const noexcept { return m_name; }

		/**
		 * Claims a slot for the calling thread, once, before its first allocation. The slot
		 * belongs to the thread, so one call covers every arena. Claim on the thread's way in
		 * and release on its way out, never mid frame. False when every slot is taken.
		 */
		static bool register_thread() noexcept;
		static void unregister_thread() noexcept;

		/**
		 * The calling thread's slot, NO_SLOT if it never claimed one. Intentionally not inlined;
		 * a compiler may cache a thread local's address across a fiber switch, and a job resumes
		 * on whichever worker took it, so each allocation asks again through a call.
		 */
		static EMBER_NOINLINE u32 thread_slot() noexcept;

		/**
		 * Bump allocation fast path; public and non-virtual so hot code holding the concrete type
		 * inlines it. Zero sized allocations take a byte, so every pointer handed out is unique
		 * until the tag is freed. The active check is an assert and compiles out of release.
		 */
		[[nodiscard]] void* allocate_fast(size_t size, size_t alignment = DEFAULT_ALIGNMENT) noexcept
		{
			EMBER_ASSERT(m_active && "allocation from an arena between end() and begin()");
			EMBER_ASSERT(is_power_of_two(alignment));

			Slot& slot		   = current_slot();
			const size_t bytes = size == 0 ? 1 : size;
			u8* aligned		   = align_up(slot.cursor, alignment);

			slot.bytes += bytes;

			// Aligning can walk off the end of the block, so the room left is only meaningful once
			// the cursor is known to be inside it. An empty slot has both pointers null and takes
			// the same path, which is how a thread's first allocation gets its first block.
			if (aligned >= slot.end || static_cast<size_t>(slot.end - aligned) < bytes) [[unlikely]]
				return allocate_from_blocks(slot, bytes, alignment);

			slot.cursor = aligned + bytes;
			return aligned;
		}

		/**
		 * True if ptr lies in a block the current tag owns. A pointer from an earlier tag reads false
		 * the moment the arena moves on, which is how a container that outlived its frame is caught.
		 */
		bool owns(const void* ptr) const noexcept
		{
			return m_heap != nullptr && m_tag != NO_TAG && m_heap->tag_of(ptr) == m_tag;
		}

		Stats stats() const noexcept;

	private:
		// A cache line each: threads bump their cursors constantly and must not share a line.
		struct alignas(EMBER_CACHE_LINE) Slot
		{
			u8* cursor	 = nullptr;
			u8* end		 = nullptr;
			size_t bytes = 0;
			u32 refills	 = 0;
			u32 blocks	 = 0;
		};

		void* do_allocate(size_t bytes, size_t alignment) noexcept override { return allocate_fast(bytes, alignment); }

		void do_deallocate(void* ptr, size_t /*bytes*/, size_t /*alignment*/) noexcept override
		{
			EMBER_ASSERT((ptr == nullptr || owns(ptr)) &&
						 "freeing memory this arena did not hand out under its current tag");
			(void)ptr; // reclaimed wholesale when the tag is freed.
		}

		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

		Slot& current_slot() noexcept
		{
			const u32 slot = thread_slot();
			EMBER_ASSERT(slot < MAX_THREADS && "arena memory belongs to registered frame threads");
			return m_slots[slot];
		}

		[[nodiscard]] void* allocate_from_blocks(Slot& slot, size_t bytes, size_t alignment) noexcept;

		void forget_blocks() noexcept;

		Slot m_slots[MAX_THREADS];
		TaggedHeap* m_heap = nullptr;
		HeapTag m_tag	   = NO_TAG;
		const char* m_name = "arena";
		bool m_active	   = false;
	};
}
