#if defined(EMBER_PLATFORM_LINUX) || defined(EMBER_PLATFORM_MACOS)
// POSIX emulation of the Win32 reserve/commit model: mmap with PROT_NONE reserves address space
// without making it accessible, and mprotect grants access on commit (physical pages materialize
// on first touch). decommit pairs madvise (return pages to the OS) with mprotect(PROT_NONE) so
// stale use of decommitted memory faults loudly, mirroring Win32 MEM_DECOMMIT semantics.
#include <ember/core/common.h>
#include <ember/core/bits.h>
#include <ember/memory/virtual_memory.h>

#include <sys/mman.h>
#include <unistd.h>

namespace
{
	using ember::virtual_memory::PageAccess;

	[[nodiscard]] int to_posix(PageAccess access) noexcept
	{
		switch (access)
		{
			case PageAccess::None:
				return PROT_NONE;
			case PageAccess::ReadOnly:
				return PROT_READ;
			case PageAccess::ReadWrite:
				return PROT_READ | PROT_WRITE;
		}

		EMBER_ASSERT(false);
		return PROT_NONE;
	}

	[[nodiscard]] bool is_page_aligned_size(size_t size) noexcept
	{
		const size_t page_size = ember::virtual_memory::page_size();
		return page_size != 0 && (size % page_size) == 0;
	}

	void assert_page_range(const void* address, size_t size) noexcept
	{
		const size_t page_size = ember::virtual_memory::page_size();

		EMBER_ASSERT(address != nullptr);
		EMBER_ASSERT(ember::is_aligned(address, page_size));
		EMBER_ASSERT(size != 0);
		EMBER_ASSERT(is_page_aligned_size(size));
	}

	[[nodiscard]] int decommit_advice() noexcept
	{
	#if defined(EMBER_PLATFORM_MACOS)
		return MADV_FREE;
	#elif defined(EMBER_PLATFORM_LINUX)
		return MADV_DONTNEED;
	#else
		#error "Unsupported POSIX virtual memory platform"
	#endif
	}
}

namespace ember::virtual_memory
{
	void* reserve(size_t size) noexcept
	{
		EMBER_ASSERT(size != 0);

		const size_t reserve_size = round_to_allocation_granularity(size);
		void* ptr				  = mmap(nullptr, reserve_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		return ptr == MAP_FAILED ? nullptr : ptr;
	}

	bool commit(void* address, size_t size, PageAccess access) noexcept
	{
		assert_page_range(address, size);
		return mprotect(address, size, to_posix(access)) == 0;
	}

	bool decommit(void* address, size_t size) noexcept
	{
		assert_page_range(address, size);

		const int advice_result  = madvise(address, size, decommit_advice());
		const int protect_result = mprotect(address, size, PROT_NONE);

		return advice_result == 0 && protect_result == 0;
	}

	bool protect(void* address, size_t size, PageAccess access) noexcept
	{
		assert_page_range(address, size);
		return mprotect(address, size, to_posix(access)) == 0;
	}

	bool release(void* address, size_t size) noexcept
	{
		assert_page_range(address, size);
		return munmap(address, size) == 0;
	}

	namespace detail
	{
		PageInfo query_page_info() noexcept
		{
			const long value = sysconf(_SC_PAGESIZE);
			EMBER_ASSERT(value > 0);

			const size_t page_size = static_cast<size_t>(value);
			return {page_size, page_size};
		}
	}
}

#endif
