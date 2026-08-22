#pragma once

#include <ember/core/common.h>
#include <ember/memory/memory.h>
#include <gpu/vulkan/common.h>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>

namespace ember::gpu::vk
{
	struct DeviceBackend;

	/**
	 * One deferred native destruction: destroyed once the frame timeline passes `value`.
	 */
	struct DeferredDestroy
	{
		enum class Kind : u8
		{
			None = 0, // zero-init state: never enqueued, asserts in the drain
			Buffer,	  // vmaDestroyBuffer(handle, allocation)
			Image,	  // vmaDestroyImage(handle, allocation)
			ImageView,
			Semaphore,
			Swapchain,
			Surface,
		};

		u64 value				 = 0;
		u64 handle				 = 0;			   // the Vk object's pointer bits, cast per kind
		VmaAllocation allocation = VK_NULL_HANDLE; // Buffer/Image only
		Kind kind				 = Kind::None;
	};

	// Non-dispatchable handles are distinct pointer types only on 64-bit platforms.
	static_assert(sizeof(void*) == 8, "Retired stores Vk handles as u64 pointer bits");

	/**
	 * The deferred destroy queue. Values are stamped from the ever-increasing timeline,
	 * so entries are enqueued already sorted: the drain walks an exact prefix and compacts
	 * only on empty.
	 *
	 * Owner-thread only, lock free.
	 */
	struct DestroyQueue
	{
		Vector<DeferredDestroy> entries{&memory::heap(MemoryTag::Graphics)};
		u32 head = 0;
	};

	/**
	 * Defers native destruction until every submit that could reference the object has
	 * retired. The overloads bind kind and handle at compile time and stamp the retire
	 * value internally. Call sites carry no lifetime decisions. Null-tolerant.
	 */
	void defer_destroy(DeviceBackend& backend, VkBuffer buffer, VmaAllocation allocation) noexcept;
	void defer_destroy(DeviceBackend& backend, VkImage image, VmaAllocation allocation) noexcept;
	void defer_destroy(DeviceBackend& backend, VkImageView view) noexcept;
	void defer_destroy(DeviceBackend& backend, VkSemaphore semaphore) noexcept;
	void defer_destroy(DeviceBackend& backend, VkSwapchainKHR swapchain) noexcept;
	void defer_destroy(DeviceBackend& backend, VkSurfaceKHR surface) noexcept;

	/**
	 * Destroys all entries whose value has provably completed.
	 */
	void drain_deferred_destroys(DeviceBackend& backend, u64 completed) noexcept;
}
