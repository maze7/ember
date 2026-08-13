#ifdef EMBER_PLATFORM_WINDOWS
	#include <ember/core/bits.h>
	#include <ember/core/common.h>
	#include <ember/memory/virtual_memory.h>

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>

namespace
{
	using ember::virtual_memory::PageAccess;

	[[nodiscard]] DWORD to_win32(PageAccess access) noexcept
	{
		switch (access)
		{
			case PageAccess::None:
				return PAGE_NOACCESS;
			case PageAccess::ReadOnly:
				return PAGE_READONLY;
			case PageAccess::ReadWrite:
				return PAGE_READWRITE;
		}

		EMBER_ASSERT(false);
		return PAGE_NOACCESS;
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
}

namespace ember::virtual_memory
{
	void* reserve(size_t size) noexcept
	{
		EMBER_ASSERT(size != 0);

		const size_t reserve_size = round_to_allocation_granularity(size);
		return VirtualAlloc(nullptr, reserve_size, MEM_RESERVE, PAGE_NOACCESS);
	}

	bool commit(void* address, size_t size, PageAccess access) noexcept
	{
		assert_page_range(address, size);
		return VirtualAlloc(address, size, MEM_COMMIT, to_win32(access)) != nullptr;
	}

	bool decommit(void* address, size_t size) noexcept
	{
		assert_page_range(address, size);
		return VirtualFree(address, size, MEM_DECOMMIT) != 0;
	}

	bool protect(void* address, size_t size, PageAccess access) noexcept
	{
		assert_page_range(address, size);

		DWORD old_protect = 0;
		return VirtualProtect(address, size, to_win32(access), &old_protect) != 0;
	}

	bool release(void* address, size_t size) noexcept
	{
		assert_page_range(address, size);
		// Win32 requires size 0 + the original base address for MEM_RELEASE; size is only asserted.
		return VirtualFree(address, 0, MEM_RELEASE) != 0;
	}

	namespace detail
	{
		PageInfo query_page_info() noexcept
		{
			SYSTEM_INFO info{};
			GetSystemInfo(&info);

			return {static_cast<size_t>(info.dwPageSize), static_cast<size_t>(info.dwAllocationGranularity)};
		}
	}
}

#endif
