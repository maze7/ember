#pragma once

#include "ember/core/common.h"
#include <ember/core/result.h>
#include <ember/memory/common.h>
#include <ember/memory/pmr/heap_resource.h>

#include <cstddef>
#include <memory_resource>
#include <utility>

namespace ember
{
	struct MemoryConfig
	{
		/**
		 * Total capacity of the shared tagged block heap.
		 * The capacity is rounded up to a whole number of blocks during init.
		 */
		size_t block_heap_capacity = 64_mb;

		/**
		 * Size of each independently claimed block.
		 *
		 * Must be a power of two and no greater than block_heap_capacity.
		 * Larger blocks reduce refill frequency but increase abandoned tail space.
		 */
		size_t block_size = 2_mb; // one thread's bump region between refills
	};

	enum class MemoryError : u8
	{
		AlreadyInit,
		MemoryTrackerInitFailed,
		BlockHeapInitFailed,
	};

	class ArenaResource;
	class BlockAllocator;
	class TaggedHeap;

	namespace memory
	{
		/**
		 * Initializes the process-wide memory services.
		 *
		 * The caller must serialize initialization and shutdown. On success, the
		 * calling OS thread owns a block-allocation slot and the Unknown tagged
		 * heap becomes the default PMR resource.
		 *
		 * A successful call must be paired with shutdown() on the same OS thread,
		 * after every other thread using engine memory has stopped.
		 */
		[[nodiscard]] Result<void, MemoryError> initialize(const MemoryConfig& config = {}) noexcept;

		/**
		 * Releases frame memory and the shared tagged heap.
		 *
		 * All jobs and external threads using these resources must be stopped
		 * first. Pointers and references into either resource become invalid.
		 *
		 * The default PMR resource remains attached to the process heap so static
		 * destruction can continue to release heap-backed objects safely.
		 */
		void shutdown() noexcept;

		/**
		 * Initializes allocator state owned by the calling OS thread.
		 *
		 * Every engine-created thread that uses the process heap must pair this with
		 * shutdown_thread(). This does not register the thread for block-backed allocation.
		 */
		void initialize_thread() noexcept;

		/**
		 * Releases allocator state owned by the calling OS thread.
		 *
		 * The thread must release all thread-local heap allocations and unregister
		 * from BlockAllocator before calling this function.
		 */
		void shutdown_thread() noexcept;

		/**
		 * Returns the shared tagged heap used by block-based transient allocators.
		 *
		 * Memory must be initialized. The returned reference remains valid until
		 * shutdown().
		 */
		[[nodiscard]] TaggedHeap& tagged_heap() noexcept;

		/**
		 * Returns the shared per-frame CPU allocator.
		 *
		 * Registered frame threads may allocate concurrently. The frame owner must
		 * call reset() only after all jobs using the current frame tag have completed.
		 * Every outstanding pointer is invalidated by that reset.
		 */
		[[nodiscard]] BlockAllocator& frame_memory() noexcept;

		/**
		 * Returns the process heap view associated with a diagnostic tag.
		 *
		 * Tags control attribution and budget reporting, not ownership. All tagged
		 * views use the same underlying process heap and compare as equal PMR resources.
		 */
		[[nodiscard]] HeapResource& heap(MemoryTag tag = MemoryTag::Unknown) noexcept;

		/**
		 * Allocates and constructs an object through a PMR resource.
		 *
		 * T must not throw during construction. The returned object must be destroyed
		 * through delete_object() using the same resource or an equal resource.
		 * Allocation failure is fatal and never returns to the caller.
		 */
		template <typename T, typename... Args>
		[[nodiscard]] T* new_object(std::pmr::memory_resource& res, Args&&... args) noexcept
		{
			void* memory = res.allocate(sizeof(T), alignof(T));
			return ::new (memory) T(std::forward<Args>(args)...);
		}

		/**
		 * Allocates and constructs an object through the process heap.
		 *
		 * The tag records subsystem attribution and does not need to accompany the
		 * resulting pointer.
		 */
		template <typename T, typename... Args> [[nodiscard]] T* new_object(MemoryTag tag, Args&&... args) noexcept
		{
			return new_object<T>(heap(tag), std::forward<Args>(args)...);
		}

		/**
		 * Destroys an object and returns its storage to a compatible PMR resource.
		 *
		 * ptr may be null. Otherwise, it must point to the most-derived T because
		 * deallocation uses sizeof(T) and alignof(T).
		 */
		template <typename T> void delete_object(std::pmr::memory_resource& mem, T* ptr) noexcept
		{
			if (ptr == nullptr)
				return;

			ptr->~T();
			mem.deallocate(ptr, sizeof(T), alignof(T));
		}

		/**
		 * Destroys a process-heap object.
		 *
		 * Any MemoryTag is compatible because every tagged HeapResource fronts
		 * the same underlying heap.
		 */
		template <typename T> void delete_object(MemoryTag tag, T* ptr) noexcept { delete_object(heap(tag), ptr); }

		/**
		 * Terminates the process after reporting an allocation failure.
		 *
		 * Engine memory resources use this path instead of exposing allocation
		 * exceptions to caller.
		 */
		[[noreturn]] void out_of_memory(size_t size, size_t alignment, MemoryTag tag) noexcept;
	}
}
