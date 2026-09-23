#pragma once

#include <ember/memory/common.h>
#include <ember/sync/spin_mutex.h>

#include <atomic>

namespace ember
{
	/** Names the lifetime a block belongs to. Everything allocated under one tag is freed together */
	using HeapTag = u64;

	inline constexpr HeapTag NO_TAG				= 0;
	inline constexpr u32 HEAP_TAG_SEQUENCE_BITS = 56;
	inline constexpr u64 HEAP_TAG_SEQUENCE_MASK = (u64{1} << HEAP_TAG_SEQUENCE_BITS) - 1;

	/**
	 * Builds a HeapTag from kind and sequence. 8 bits for kind, 54 for sequence.
	 * Kind must be non-zero.
	 */
	[[nodiscard]] constexpr HeapTag heap_tag(u8 kind, u64 sequence = 0) noexcept
	{
		EMBER_ASSERT(kind != 0);
		EMBER_ASSERT(sequence <= HEAP_TAG_SEQUENCE_MASK);

		return (static_cast<HeapTag>(kind) << HEAP_TAG_SEQUENCE_BITS) | (sequence & HEAP_TAG_SEQUENCE_MASK);
	}

	/** Retrieves the `kind` bits from a HeapTag */
	[[nodiscard]] constexpr u8 kind(HeapTag tag) noexcept { return static_cast<u8>(tag >> HEAP_TAG_SEQUENCE_BITS); }

	/** Retrieves the `sequence` bits from a HeapTag */
	[[nodiscard]] constexpr u8 sequence(HeapTag tag) noexcept { return tag & HEAP_TAG_SEQUENCE_MASK; }

	/**
	 * The shared block pool behind every allocator, following the tagged heap design in Christian
	 * Gyrling's GDC 2015 talk: (https://www.gdcvault.com/play/1022186/parallelizing-the-naughty-dog-engine)
	 *
	 * One reservation is cut into fixed blocks, each owned by a tag. Allocators take blocks and bump
	 * allocate inside them. All blocks for a given tag are released at once. This block allocator backs
	 * all other allocators within the engine except for the rpmalloc general heap.
	 */
	class TaggedHeap final
	{
	public:
		static constexpr MemoryTag MEMORY_TAG = MemoryTag::Engine;

		constexpr TaggedHeap() noexcept = default;
		~TaggedHeap() noexcept;

		TaggedHeap(const TaggedHeap&)			 = delete;
		TaggedHeap& operator=(const TaggedHeap&) = delete;

		/**
		 * Initializes the TaggedHeap, allocates underlying memory.
		 * block_size must be a power of two; capacity rounds up to whole blocks.
		 */
		[[nodiscard]] bool init(size_t capacity, size_t block_size) noexcept;

		/** Every tag must have been freed. Asserts on a leak in debug and releases anyway. */
		void shutdown() noexcept;

		/**
		 * Claim consecutive blocks by a tag. Returns nullptr when no run that long is free.
		 * The lowest free run wins. Any thread. Intended for higher-level allocators to request
		 * backing blocks, not per allocation.
		 */
		[[nodiscard]] void* allocate(u32 count, HeapTag tag) noexcept;

		/** Returns every block owned by tag and reports how many. */
		u32 free(HeapTag tag) noexcept;

		/** The tag owning the block ptr lies in; NO_TAG for a free block or a foreign pointer. */
		[[nodiscard]] HeapTag tag_of(const void* ptr) const noexcept;

		/** True if ptr lies inside the reservation, whoever owns that block. */
		[[nodiscard]] bool owns(const void* ptr) const noexcept
		{
			const u8* p = static_cast<const u8*>(ptr);
			return p >= m_base && p < m_base + m_capacity;
		}

		[[nodiscard]] u64 block_size() const noexcept { return m_block_size; }
		[[nodiscard]] u64 capacity() const noexcept { return m_capacity; }
		[[nodiscard]] u32 block_count() const noexcept { return m_block_count; }
		[[nodiscard]] u32 blocks_in_use() const noexcept { return m_blocks_in_use.load(std::memory_order_relaxed); }
		[[nodiscard]] u64 used() const noexcept { return blocks_in_use() * m_block_size; }
		[[nodiscard]] size_t peak() const noexcept
		{
			return m_peak_blocks.load(std::memory_order_relaxed) * m_block_size;
		}

		/** Blocks a tag owns right now. A linear scan, intended for telemetry and tests. */
		[[nodiscard]] u32 blocks_owned(HeapTag tag) const noexcept;

	private:
		[[nodiscard]] u32 find_run(u32 count) const noexcept;
		[[nodiscard]] bool is_free(u32 block) const noexcept { return ((m_free[block / 64] >> (block % 64)) & 1) != 0; }

		SpinMutex m_lock;
		u64* m_free					 = nullptr; // one bit per block, set while free; under the lock
		std::atomic<HeapTag>* m_tags = nullptr; // owner per block, NO_TAG while free; written under the lock

		u8* m_base			= nullptr;
		size_t m_capacity	= 0;
		size_t m_block_size = 0;
		u32 m_block_count	= 0;
		u32 m_word_count	= 0;
		u32 m_hint			= 0; // no free bit lives below this word; under the lock
		std::atomic<u32> m_blocks_in_use{0};
		std::atomic<u32> m_peak_blocks{0};
	};
}
