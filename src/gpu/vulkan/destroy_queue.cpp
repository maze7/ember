#include <gpu/vulkan/destroy_queue.h>
#include <platform/vulkan/wsi.h>

#include <ember/sync/thread.h>
#include <gpu/vulkan/device_state.h>

#include <vulkan/vulkan_core.h>

namespace ember::gpu::vk
{
	namespace
	{
		void enqueue(Backend& backend, DeferredDestroy dead) noexcept
		{
			EMBER_ASSERT(backend.owner_thread == current_thread_id());
			EMBER_ASSERT(dead.kind != DeferredDestroy::Kind::None);

			// Null-tolerant like vkDestroy* itself, so call sites stay branch free.
			if (dead.handle == 0)
				return;

			dead.value = backend.frame.timeline_value + 1;
			backend.deferred.entries.push_back(dead);
		}
	}

	void defer_destroy(Backend& backend, VkBuffer buffer, VmaAllocation allocation) noexcept
	{
		enqueue(
			backend,
			{
				.handle		= reinterpret_cast<u64>(buffer),
				.allocation = allocation,
				.kind		= DeferredDestroy::Kind::Buffer,
			});
	}

	void defer_destroy(Backend& backend, VkImage image, VmaAllocation allocation) noexcept
	{
		enqueue(
			backend,
			{.handle = reinterpret_cast<u64>(image), .allocation = allocation, .kind = DeferredDestroy::Kind::Image});
	}

	void defer_destroy(Backend& backend, VkImageView view) noexcept
	{
		enqueue(backend, {.handle = reinterpret_cast<u64>(view), .kind = DeferredDestroy::Kind::ImageView});
	}

	void defer_destroy(Backend& backend, VkSemaphore semaphore) noexcept
	{
		enqueue(backend, {.handle = reinterpret_cast<u64>(semaphore), .kind = DeferredDestroy::Kind::Semaphore});
	}

	void defer_destroy(Backend& backend, VkSwapchainKHR swapchain) noexcept
	{
		enqueue(backend, {.handle = reinterpret_cast<u64>(swapchain), .kind = DeferredDestroy::Kind::Swapchain});
	}

	void defer_destroy(Backend& backend, VkSurfaceKHR surface) noexcept
	{
		enqueue(backend, {.handle = reinterpret_cast<u64>(surface), .kind = DeferredDestroy::Kind::Surface});
	}

	void drain_deferred_destroys(Backend& backend, u64 completed) noexcept
	{
		EMBER_ASSERT(backend.owner_thread == current_thread_id());

		Vector<DeferredDestroy>& entries = backend.deferred.entries;
		u32& head						 = backend.deferred.head;

		while (head < entries.size() && entries[head].value <= completed)
		{
			const DeferredDestroy& dead = entries[head++];

			switch (dead.kind) // no default: -Wswitch keeps every Kind handled
			{
				case DeferredDestroy::Kind::None:
					EMBER_ASSERT(false && "zero-initialized DeferredDelete reached the drain");
					break;

				case DeferredDestroy::Kind::Buffer:
					vmaDestroyBuffer(backend.context.allocator, reinterpret_cast<VkBuffer>(dead.handle), dead.allocation);
					break;

				case DeferredDestroy::Kind::Image:
					vmaDestroyImage(backend.context.allocator, reinterpret_cast<VkImage>(dead.handle), dead.allocation);
					break;

				case DeferredDestroy::Kind::ImageView:
					vkDestroyImageView(backend.context.device, reinterpret_cast<VkImageView>(dead.handle), nullptr);
					break;

				case DeferredDestroy::Kind::Semaphore:
					vkDestroySemaphore(backend.context.device, reinterpret_cast<VkSemaphore>(dead.handle), nullptr);
					break;

				case DeferredDestroy::Kind::Swapchain:
					vkDestroySwapchainKHR(backend.context.device, reinterpret_cast<VkSwapchainKHR>(dead.handle), nullptr);
					break;

				case DeferredDestroy::Kind::Surface:
					platform::vk::destroy_surface(backend.context.instance, reinterpret_cast<VkSurfaceKHR>(dead.handle));
					break;
			}
		}

		// Compact only when fully drained: erasing from the front would shuffle every
		// surviving entry, and the queue empties naturally as frames retire.
		if (head == entries.size())
		{
			entries.clear();
			head = 0;
		}
	}
}
