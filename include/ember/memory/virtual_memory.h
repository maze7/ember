#pragma once

#include <ember/core/common.h>

namespace ember::virtual_memory
{
	enum class PageAccess : u8
	{
		None,
		ReadOnly,
		ReadWrite,
	};

	struct PageInfo
	{
		size_t page_size			  = 0;
		size_t allocation_granularity = 0;
	};

	[[nodiscard]] PageInfo page_info() noexcept;
	[[nodiscard]] size_t page_size() noexcept;
	[[nodiscard]]  size_t allocation_granularity() noexcept;

	[[nodiscard]] size_t round_to_page_size(size_t size) noexcept;
	[[nodiscard]] size_t round_to_allocation_granularity(size_t size) noexcept;

	// size is rounded up to allocation_granularity(); the reservation extends to the rounded size.
	[[nodiscard]] void* reserve(size_t size) noexcept;
	[[nodiscard]] bool commit(void* address, size_t size, PageAccess access = PageAccess::ReadWrite) noexcept;
	[[nodiscard]] bool decommit(void* address, size_t size) noexcept;
	[[nodiscard]] bool protect(void* address, size_t size, PageAccess access) noexcept;
	// size must be the granularity-rounded size of the original reservation.
	[[nodiscard]] bool release(void* address, size_t size) noexcept;

	// Reserves round_to_allocation_granularity(size) and commits round_to_page_size(size).
	[[nodiscard]] void* reserve_and_commit(size_t size, PageAccess access = PageAccess::ReadWrite) noexcept;
}
