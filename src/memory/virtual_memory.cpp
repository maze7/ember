#include <ember/core/bits.h>
#include <ember/memory/virtual_memory.h>

namespace ember::virtual_memory
{
	namespace detail
	{
		PageInfo query_page_info() noexcept;
	}

	PageInfo page_info() noexcept
	{
		static const PageInfo s_info = detail::query_page_info(); // one-time, thread-safe (magic static)
		return s_info;
	}

	size_t page_size() noexcept
	{
		return page_info().page_size;
	}

	size_t allocation_granularity() noexcept
	{
		return page_info().allocation_granularity;
	}

	size_t round_to_page_size(size_t size) noexcept
	{
		return align_up(size, page_size());
	}

	size_t round_to_allocation_granularity(size_t size) noexcept
	{
		return align_up(size, allocation_granularity());
	}

	void* reserve_and_commit(size_t size, PageAccess access) noexcept
	{
		const size_t reserve_size = round_to_allocation_granularity(size);
		const size_t commit_size  = round_to_page_size(size);

		void* ptr = reserve(reserve_size);
		if (ptr == nullptr)
			return nullptr;

		if (!commit(ptr, commit_size, access))
		{
			(void)release(ptr, reserve_size);
			return nullptr;
		}

		return ptr;
	}
}
