#include <gpu/vulkan/destroy_queue.h>
#include <platform/vulkan/wsi.h>

#include <ember/sync/thread.h>
#include <gpu/vulkan/backend.h>
#include <vulkan/vulkan_core.h>

namespace ember::gpu::vk
{
	namespace
	{
		/// Destroy sites retire one resource kind per entry, so the mask names the pool.
		void release_pool_slot(ResourcePools& pools, HeapArray mask, u16 slot) noexcept
		{
			if ((mask & (HeapArray::Sampled | HeapArray::Storage)) != HeapArray::None)
				pools.textures.release_slot(slot);
			else if ((mask & HeapArray::Sampler) != HeapArray::None)
				pools.samplers.release_slot(slot);
			else
				pools.buffers.release_slot(slot);
		}
	}

	void DestroyQueue::bind(const FrameState& frame) noexcept
	{
		EMBER_ASSERT(m_frame == nullptr);
		m_frame = &frame;
		m_owner = current_thread_id();
	}

	void DestroyQueue::enqueue(Entry dead) noexcept
	{
		EMBER_ASSERT(m_frame != nullptr && "DestroyQueue used before bind");
		EMBER_ASSERT(m_owner == current_thread_id());
		EMBER_ASSERT(dead.kind != Kind::None);

		// The pending value: what the next submit will signal. Read live here so out-of-frame
		// defers (one-off staging, load screens) stamp against the submit that actually consumes them.
		dead.value = m_frame->timeline_value + 1;
		m_entries.push_back(dead);
	}

	void DestroyQueue::destroy(VkBuffer buffer, VmaAllocation allocation) noexcept
	{
		if (buffer == VK_NULL_HANDLE)
			return;

		enqueue({
			.handle		= reinterpret_cast<void*>(buffer),
			.allocation = allocation,
			.kind		= Kind::Buffer,
		});
	}

	void DestroyQueue::destroy(VkImage image, VmaAllocation allocation) noexcept
	{
		if (image == VK_NULL_HANDLE)
			return;

		enqueue({
			.handle		= reinterpret_cast<void*>(image),
			.allocation = allocation,
			.kind		= Kind::Image,
		});
	}

	void DestroyQueue::destroy(VkImageView image_view) noexcept
	{
		if (image_view == VK_NULL_HANDLE)
			return;

		enqueue({
			.handle = reinterpret_cast<void*>(image_view),
			.kind	= Kind::ImageView,
		});
	}

	void DestroyQueue::destroy(VkSampler sampler) noexcept
	{
		if (sampler == VK_NULL_HANDLE)
			return;

		enqueue({
			.handle = sampler,
			.kind	= Kind::Sampler,
		});
	}

	void DestroyQueue::destroy(VkSemaphore semaphore) noexcept
	{
		if (semaphore == VK_NULL_HANDLE)
			return;

		enqueue({
			.handle = reinterpret_cast<void*>(semaphore),
			.kind	= Kind::Semaphore,
		});
	}

	void DestroyQueue::destroy(VkPipeline pipeline) noexcept
	{
		if (pipeline == VK_NULL_HANDLE)
			return;

		enqueue({
			.handle = reinterpret_cast<void*>(pipeline),
			.kind	= Kind::Pipeline,
		});
	}

	void DestroyQueue::destroy(VkSwapchainKHR swapchain) noexcept
	{
		if (swapchain == VK_NULL_HANDLE)
			return;

		enqueue({
			.handle = reinterpret_cast<void*>(swapchain),
			.kind	= Kind::Swapchain,
		});
	}

	void DestroyQueue::destroy(VkSurfaceKHR surface) noexcept
	{
		if (surface == VK_NULL_HANDLE)
			return;

		enqueue({
			.handle = reinterpret_cast<void*>(surface),
			.kind	= Kind::Surface,
		});
	}

	void DestroyQueue::reset_slot(u16 slot, HeapArray heap_mask) noexcept
	{
		// Slot 0 belongs to the fallbacks for the device's whole life; resetting
		// it means a handle went through destroy() that never should have.
		EMBER_ASSERT(slot != 0);

		enqueue({.kind = Kind::HeapSlot, .heap_mask = heap_mask, .slot = slot});
	}

	void DestroyQueue::drain(const Context& ctx, DescriptorHeap& heap, ResourcePools& pools, u64 completed) noexcept
	{
		EMBER_ASSERT(m_owner == current_thread_id());

		while (m_head < m_entries.size() && m_entries[m_head].value <= completed)
		{
			const Entry& dead = m_entries[m_head++];

			switch (dead.kind) // no default: -Wswitch keeps every Kind handled
			{
				case Kind::None:
					EMBER_ASSERT(false && "zero-initialized Entry reached the drain");
					break;

				case Kind::Buffer:
					vmaDestroyBuffer(ctx.allocator, reinterpret_cast<VkBuffer>(dead.handle), dead.allocation);
					break;

				case Kind::Image:
					vmaDestroyImage(ctx.allocator, reinterpret_cast<VkImage>(dead.handle), dead.allocation);
					break;

				case Kind::ImageView:
					vkDestroyImageView(ctx.device, reinterpret_cast<VkImageView>(dead.handle), nullptr);
					break;

				case Kind::Sampler:
					vkDestroySampler(ctx.device, reinterpret_cast<VkSampler>(dead.handle), nullptr);
					break;

				case Kind::Semaphore:
					vkDestroySemaphore(ctx.device, reinterpret_cast<VkSemaphore>(dead.handle), nullptr);
					break;

				case Kind::Pipeline:
					vkDestroyPipeline(ctx.device, reinterpret_cast<VkPipeline>(dead.handle), nullptr);
					break;

				case Kind::Swapchain:
					vkDestroySwapchainKHR(ctx.device, reinterpret_cast<VkSwapchainKHR>(dead.handle), nullptr);
					break;

				case Kind::Surface:
					vkDestroySurfaceKHR(ctx.instance, reinterpret_cast<VkSurfaceKHR>(dead.handle), nullptr);
					break;

				case Kind::HeapSlot:
					heap.reset_slot(ctx, dead.slot, dead.heap_mask);
					release_pool_slot(pools, dead.heap_mask, dead.slot);
					break;
			}
		}

		// Compact only when fully drained: erasing from the front would shuffle every
		// surviving entry, and the queue empties naturally as frames retire.
		if (m_head == m_entries.size())
		{
			m_entries.clear();
			m_head = 0;
		}
		else if (m_head >= 256) // amortized: survivors span only frames_in_flight frames
		{
			m_entries.erase(m_entries.begin(), m_entries.begin() + m_head);
			m_head = 0;
		}
	}
}
