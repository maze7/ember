#pragma once

#include <ember/memory/common.h>

#include <atomic>

namespace ember
{
	/**
	 * Names the owner of a block. Everything allocated under one tag is freed together, and
	 * nothing else is ever freed: there is no free(ptr) here. A tag is any non zero u64, usually a
	 * lifetime kind packed with a sequence number so each frame's memory gets its own.
	 *
	 * Not to be confused with MemoryTag, which says which budget an allocation is charged to. This
	 * one says when it dies.
	 */
	using HeapTag = u64;

	inline constexpr HeapTag NO_TAG = 0;

	/// Packs a lifetime kind (non zero) and a sequence number, so per frame stage memory is named
	/// by (kind, frame) and the frames do not collide.
	[[nodiscard]] constexpr HeapTag heap_tag(u32 kind, u32 sequence = 0) noexcept
	{
		EMBER_ASSERT(kind != 0);
		return (static_cast<HeapTag>(kind) << 32) | sequence;
	}

	/**
	 * The shared block pool behind every block allocator, following the tagged heap in Christian
	 * Gyrling's GDC 2015 talk. One reservation is cut into fixed blocks, each owned by a tag;
	 * allocators take blocks and bump inside them, and a whole tag is released at once.
	 *
	 * Sharing one pool is the point: sizing each allocator for its own worst case is what the
	 * tagged heap exists to avoid. A tag that needs twenty blocks this frame can have them while
	 * the others use one each.
	 *
	 * Blocks are claimed and released with atomics, no locks. Claiming scans the tag table for a
	 * free run, which is cold code: it runs once per block, not once per allocation.
	 */
	class TaggedHeap final
	{
	public:
		constexpr TaggedHeap() noexcept = default;
		~TaggedHeap() noexcept;

		TaggedHeap(const TaggedHeap&)			 = delete;
		TaggedHeap& operator=(const TaggedHeap&) = delete;

		/// block_size must be a power of two; capacity rounds up to a whole number of blocks and is
		/// committed here, so taking a block is never a system call.
		[[nodiscard]] bool init(size_t capacity, size_t block_size, MemoryTag tag = MemoryTag::Engine) noexcept;
		void shutdown() noexcept;

		/**
		 * count consecutive blocks owned by tag, or nullptr when no run that long is free. Any
		 * thread, any time.
		 */
		[[nodiscard]] void* allocate_blocks(u32 count, HeapTag tag) noexcept;

		/**
		 * Releases every block owned by tag. The tag's sync point: nothing may be allocating under
		 * it, and no pointer into its blocks may be read afterwards.
		 */
		void free_blocks(HeapTag tag) noexcept;

		// True if ptr lies inside the reservation. Loose on purpose, so it stays valid whoever owns
		// the block; used to catch memory from elsewhere being freed through a block allocator.
		[[nodiscard]] bool owns(const void* ptr) const noexcept
		{
			const u8* p = static_cast<const u8*>(ptr);
			return p >= m_base && p < m_base + m_capacity;
		}

		[[nodiscard]] size_t block_size() const noexcept { return m_block_size; }
		[[nodiscard]] size_t capacity() const noexcept { return m_capacity; }
		[[nodiscard]] u32 block_count() const noexcept { return m_block_count; }
		[[nodiscard]] MemoryTag memory_tag() const noexcept { return m_tag; }

		[[nodiscard]] u32 blocks_in_use() const noexcept { return m_blocks_in_use.load(std::memory_order_relaxed); }
		[[nodiscard]] size_t used() const noexcept { return blocks_in_use() * m_block_size; }
		[[nodiscard]] size_t peak() const noexcept { return m_peak_blocks.load(std::memory_order_relaxed) * m_block_size; }

	private:
		[[nodiscard]] bool window_is_free(u32 first, u32 count) const noexcept;
		[[nodiscard]] u32 claim(u32 first, u32 count, HeapTag tag) noexcept;
		void release(u32 first, u32 count) noexcept;

		std::atomic<HeapTag>* m_block_tags	  = nullptr; // NO_TAG while the block is free
		u8*					  m_base		  = nullptr;
		size_t				  m_capacity	  = 0;
		size_t				  m_block_size	  = 0;
		u32					  m_block_count	  = 0;
		std::atomic<u32>	  m_hint		  = 0; // where the last claim finished, so scans start warm
		std::atomic<u32>	  m_blocks_in_use = 0;
		std::atomic<u32>	  m_peak_blocks	  = 0;
		MemoryTag			  m_tag			  = MemoryTag::Unknown;
	};
}
