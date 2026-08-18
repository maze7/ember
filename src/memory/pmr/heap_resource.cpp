#include <ember/core/bits.h>
#include <ember/core/common.h>
#include <ember/core/logger.h>
#include <ember/memory/memory.h>
#include <ember/memory/memory_tracker.h>
#include <ember/memory/pmr/heap_resource.h>

#include <array>
#include <mutex>
#include <source_location>

#if EMBER_USE_RPMALLOC
	#include <rpmalloc.h>
#else
	#include <cstdlib>
	#include <cstring>
	#if defined(_MSC_VER)
		#include <malloc.h>
	#elif defined(EMBER_PLATFORM_MACOS)
		#include <malloc/malloc.h>
	#elif defined(EMBER_PLATFORM_LINUX)
		#include <malloc.h>
	#endif
#endif

namespace ember
{
	namespace
	{
#if EMBER_USE_RPMALLOC
		void rpmalloc_error_callback(const char* message)
		{
			// Memory diagnostics bypass stripped log macros: tracking-gated or fatal
			// and too rare to strip.
			Logger::error(
				std::source_location::current(), "rpmalloc: {}", message != nullptr ? message : "unknown error");
		}

		constinit rpmalloc_interface_t s_rpmalloc_interface = {
			.memory_map		   = nullptr,
			.memory_commit	   = nullptr,
			.memory_decommit   = nullptr,
			.memory_unmap	   = nullptr,
			.map_fail_callback = nullptr,
			.error_callback	   = rpmalloc_error_callback,
		};

		constinit std::once_flag s_rpmalloc_once;
#endif

		// call_once's fast path is a single acquire load, cheap enough to keep every
		// allocation safe before memory_init() (static initializers, tests).
		void ensure_initialized() noexcept
		{
#if EMBER_USE_RPMALLOC
			std::call_once(
				s_rpmalloc_once,
				[]()
				{
					if (rpmalloc_initialize(&s_rpmalloc_interface) != 0) [[unlikely]]
					{
						Logger::error(std::source_location::current(), "rpmalloc init failed");
						EMBER_DEBUG_BREAK();
						std::abort();
					}
				});
#endif
		}

#if !EMBER_USE_RPMALLOC
		[[nodiscard]] size_t allocation_size(size_t size) noexcept { return size == 0 ? 1 : size; }
#endif // EMBER_USE_RPMALLOC

		[[nodiscard]] void* mem_alloc(size_t size, size_t align, bool zeroed) noexcept
		{
#if EMBER_USE_RPMALLOC
			if (align <= DEFAULT_ALIGNMENT)
				return zeroed ? rpzalloc(size) : rpmalloc(size);

			return zeroed ? rpaligned_zalloc(align, size) : rpaligned_alloc(align, size);
#else
	#if defined(_MSC_VER)
			const size_t actual_align = align <= DEFAULT_ALIGNMENT ? DEFAULT_ALIGNMENT : align;
			const size_t rounded_size = align_up(allocation_size(size), actual_align);
			void* ptr				  = _aligned_malloc(rounded_size, actual_align);

			if (zeroed && ptr != nullptr)
				std::memset(ptr, 0, rounded_size);
			return ptr;
	#else
			if (align <= DEFAULT_ALIGNMENT)
				return zeroed ? std::calloc(1, allocation_size(size)) : std::malloc(allocation_size(size));

			const size_t rounded_size = align_up(allocation_size(size), align);
			void* ptr				  = std::aligned_alloc(align, rounded_size);
			if (zeroed && ptr != nullptr)
				std::memset(ptr, 0, rounded_size);

			return ptr;
	#endif
#endif // EMBER_USE_RPMALLOC
		}

		void mem_free(void* ptr) noexcept
		{
#if EMBER_USE_RPMALLOC
			rpfree(ptr);
#else
	#if defined(_MSC_VER)
			_aligned_free(ptr);
	#else
			std::free(ptr);
	#endif
#endif // EMBER_USE_RPMALLOC
		}

		[[nodiscard]] void* mem_realloc(void* ptr, size_t new_size) noexcept
		{
#if EMBER_USE_RPMALLOC
			return rprealloc(ptr, new_size);
#else
	#if defined(_MSC_VER)
			return _aligned_realloc(ptr, allocation_size(new_size), DEFAULT_ALIGNMENT);
	#else
			return std::realloc(ptr, allocation_size(new_size));
	#endif
#endif // EMBER_USE_RPMALLOC
		}

		// One heap view per tag, in MemoryTag declaration order. memory_resource's  destructor
		// is not constexpr, so these are aggregate-initialized directly rather than through a factory.
		constinit std::array<HeapResource, static_cast<size_t>(MemoryTag::Count)> s_heaps = {
			HeapResource{MemoryTag::Unknown},
			HeapResource{MemoryTag::Engine},
			HeapResource{MemoryTag::Graphics},
			HeapResource{MemoryTag::Audio},
			HeapResource{MemoryTag::Physics},
			HeapResource{MemoryTag::ECS},
			HeapResource{MemoryTag::Gameplay},
			HeapResource{MemoryTag::Assets},
			HeapResource{MemoryTag::Scripting},
			HeapResource{MemoryTag::Network},
			HeapResource{MemoryTag::Platform},
			HeapResource{MemoryTag::Input},
			HeapResource{MemoryTag::Tools},
			HeapResource{MemoryTag::Strings},
		};

		// Ensure we don't drift from MemoryTag
		static_assert(
			static_cast<size_t>(MemoryTag::Count) == 14, "New MemoryTag: add its HeapResource to s_heaps above.");
	}

	// Tracking accounts the usable (block) size rather than the requested size: it mirrors
	// real heap consumption and lets unsized C-hook frees account correctly.

	void* HeapResource::do_allocate(size_t bytes, size_t alignment) noexcept
	{
		EMBER_ASSERT(is_power_of_two(alignment));
		ensure_initialized();

		void* ptr = mem_alloc(bytes, alignment, false);
		if (ptr == nullptr) [[unlikely]]
			out_of_memory(bytes, alignment, m_tag);

		EMBER_MEM_TRACK_ALLOC(ptr, usable_size(ptr), m_tag, true);
		return ptr;
	}

	void HeapResource::do_deallocate(void* ptr, size_t /*bytes*/, size_t /*alignment*/) noexcept
	{
		// rpmalloc frees without size, so the sized and unsized paths are one path.
		deallocate_unsized(ptr);
	}

	bool HeapResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
	{
		// Containers use equality to decide whether buffers may be adopted or move-assign
		// and swap; any two tagged heaps qualify. Cold path, and the loop avoids RTTI.
		for (const HeapResource& heap : s_heaps)
			if (&heap == &other)
				return true;

		return false;
	}

	void* HeapResource::allocate_zeroed(size_t size, size_t alignment) noexcept
	{
		EMBER_ASSERT(is_power_of_two(alignment));
		ensure_initialized();

		void* ptr = mem_alloc(size, alignment, true);
		if (ptr == nullptr) [[unlikely]]
			out_of_memory(size, alignment, m_tag);

		EMBER_MEM_TRACK_ALLOC(ptr, usable_size(ptr), m_tag, false);
		return ptr;
	}

	void* HeapResource::reallocate(void* ptr, size_t new_size) noexcept
	{
		ensure_initialized();

		if (ptr != nullptr)
			EMBER_MEM_TRACK_FREE_UNPOISONED(ptr, usable_size(ptr));

		void* new_ptr = mem_realloc(ptr, new_size);
		if (new_ptr == nullptr) [[unlikely]]
			out_of_memory(new_size, DEFAULT_ALIGNMENT, m_tag);

		EMBER_MEM_TRACK_ALLOC(new_ptr, usable_size(new_ptr), m_tag, false);
		return new_ptr;
	}

	void HeapResource::deallocate_unsized(void* ptr) noexcept
	{
		if (ptr == nullptr)
			return;

		EMBER_MEM_TRACK_FREE(ptr, usable_size(ptr));
		mem_free(ptr);
	}

	size_t HeapResource::usable_size(void* ptr) noexcept
	{
		if (ptr == nullptr)
			return 0;

#if EMBER_USE_RPMALLOC
		return rpmalloc_usable_size(ptr);
#else
	#if defined(_MSC_VER)
		// Misreports over-aligned blocks; acceptable for the sanitizer-only escape hatch.
		return _aligned_msize(ptr, DEFAULT_ALIGNMENT, 0);
	#elif defined(EMBER_PLATFORM_MACOS)
		return malloc_size(ptr);
	#elif defined(EMBER_PLATFORM_LINUX)
		return malloc_usable_size(ptr);
	#else
		return 0;
	#endif
#endif // EMBER_USE_RPMALLOC
	}

	namespace memory
	{
		HeapResource& heap(MemoryTag tag) noexcept
		{
			const size_t index = static_cast<size_t>(tag);
			EMBER_ASSERT(index < s_heaps.size());
			return s_heaps[index < s_heaps.size() ? index : 0];
		}

		void initialize_thread() noexcept
		{
			ensure_initialized();

	#if EMBER_USE_RPMALLOC
			if (!rpmalloc_is_thread_initialized())
				rpmalloc_thread_initialize();
	#endif // EMBER_USE_RPMALLOC
		}

		void shutdown_thread() noexcept
		{
	#if EMBER_USE_RPMALLOC
			if (rpmalloc_is_thread_initialized())
				rpmalloc_thread_finalize();
	#endif // EMBER_USE_RPMALLOC
		}
	}
}
