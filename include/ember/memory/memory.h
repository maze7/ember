#pragma once

#include <ember/memory/common.h>
#include <ember/memory/pmr/heap_resource.h>

#include <cstddef>
#include <memory_resource>
#include <new>
#include <utility>

namespace ember
{
	struct MemoryConfig
	{
		size_t frame_arena_reserve = 128_mb;
		size_t frame_arena_commit  = 1_mb;
	};

	class ArenaResource;

	class MemorySystem final
	{
	public:
		explicit MemorySystem(const MemoryConfig& config = {}) noexcept;

		~MemorySystem() noexcept;

		MemorySystem(const MemorySystem&)			 = delete;
		MemorySystem& operator=(const MemorySystem&) = delete;
		MemorySystem(MemorySystem&&)				 = delete;
		MemorySystem& operator=(MemorySystem&&)		 = delete;

		[[nodiscard]] explicit operator bool() const noexcept { return m_initialized; }

	private:
		bool m_initialized = false;
	};

	namespace memory
	{
		// rpmalloc keeps per-thread caches; every engine-created thread calls these at startup / shutdown
		void initialize_thread() noexcept;
		void shutdown_thread() noexcept;

		[[nodiscard]] ArenaResource& frame_arena() noexcept;

		/**
		 * The process heap viewed through a given tag. Pass to containers and new_object at subsystem wiring
		 * points; everything beneath inherits the attribution.
		 */
		[[nodiscard]] HeapResource& heap(MemoryTag tag = MemoryTag::Unknown) noexcept;

		/**
		 * Engine-flavoured polymorphic_allocator::new_object/delete_object: noexcept, aborts on OOM.
		 * For new objects not owned by a container (subsystem impls behind points).
		 */
		template <typename T, typename... Args>
		[[nodiscard]] T* new_object(std::pmr::memory_resource& res, Args&&... args) noexcept
		{
			void* memory = res.allocate(sizeof(T), alignof(T));
			return ::new (memory) T(std::forward<Args>(args)...);
		}

		/**
		 * Engine-flavoured polymorphic_allocator::new_object/delete_object: noexcept, aborts on OOM.
		 * For new objects not owned by a container (subsystem impls behind points).
		 */
		template <typename T, typename... Args> [[nodiscard]] T* new_object(MemoryTag tag, Args&&... args) noexcept
		{
			return new_object<T>(heap(tag), std::forward<Args>(args)...);
		}

		// ptr must point at the most-derived type: deallocation is sized with sizeof(T).
		template <typename T> void delete_object(std::pmr::memory_resource& mem, T* ptr) noexcept
		{
			if (ptr == nullptr)
				return;

			ptr->~T();
			mem.deallocate(ptr, sizeof(T), alignof(T));
		}

		// ptr must point at the most-derived type: deallocation is sized with sizeof(T).
		template <typename T> void delete_object(MemoryTag tag, T* ptr) noexcept { delete_object(heap(tag), ptr); }
	}

	/// Genuine OOM is engine-fatal: log, break, abort. Resources call this
	/// instead of throwing, so PMR containers never observe allocation failure.
	[[noreturn]] void out_of_memory(size_t size, size_t alignment, MemoryTag tag) noexcept;
}
