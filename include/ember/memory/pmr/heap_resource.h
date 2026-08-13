#pragma once

#include <ember/memory/common.h>
#include <memory_resource>

namespace ember
{
	/**
	 * The process heap (rpmalloc), viewed through one statically-allocated instance
	 * per MemoryTag. Every instance fronts the same underlying heap, so all HeapResources
	 * compare as equal and a block may be freed through any of them. The tracker's live map
	 * correctly attributes each block to its allocating tag regardless.
	 *
	 * Thread-safe.
	 */
	class HeapResource final : public std::pmr::memory_resource
	{
	public:
		constexpr explicit HeapResource(MemoryTag tag) noexcept : m_tag(tag) {}

		HeapResource(const HeapResource&)			 = delete;
		HeapResource& operator=(const HeapResource&) = delete;

		/**
		 * Malloc-shaped extras for C libraries (third_party hooks). C callbacks usually
		 * don't provide a size on free and expect calloc/realloc semantics, which
		 * memory_resource does not model. Engine code generally shouldn't use these.
		 */
		[[nodiscard]] void* allocate_zeroed(size_t size, size_t alignment = DEFAULT_ALIGNMENT) noexcept;

		/** System-allocator builds can only preserve the default alignment through realloc. */
		[[nodiscard]] void* reallocate(void* ptr, size_t new_size) noexcept;

		void deallocate_unsized(void* ptr) noexcept;

		[[nodiscard]] static size_t usable_size(void* ptr) noexcept;

		[[nodiscard]] MemoryTag tag() const noexcept { return m_tag; }

	private:
		void* do_allocate(size_t bytes, size_t alignment) noexcept override;
		void do_deallocate(void* ptr, size_t bytes, size_t alignment) noexcept override;
		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

		MemoryTag m_tag;
	};
}
