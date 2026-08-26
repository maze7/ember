#pragma once

#include <ember/core/common.h>
#include <ember/memory/memory.h>
#include <gpu/vulkan/common.h>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>

namespace ember::gpu
{
	struct Backend;
	struct FrameState;
}

namespace ember::gpu::vk
{
	/**
	 * Deferred native destruction, keyed to the frame timeline.
	 *
	 * destroy() captures the raw objects and stamps them with the value the next submit  will signal.
	 * drain() destroys the exact prefix proven complete.
	 *
	 * Values are minted monotonically, so entries arrive already sorted: the drain walks a prefix
	 * and compacts lazily. Owner-thread only (asserted), lock free.
	 */
	class DestroyQueue
	{
	public:
		/// Wires the DestroyQueue to the submission clock through the frame state it belongs to.
		void bind(const FrameState& frame) noexcept;

		/// Null-tolerant like vkDestory* itself, so call sites stay branch free.
		void destroy(VkBuffer buffer, VmaAllocation allocation) noexcept;
		void destroy(VkImage image, VmaAllocation allocation) noexcept;
		void destroy(VkImageView view) noexcept;
		void destroy(VkSemaphore semaphore) noexcept;
		void destroy(VkSwapchainKHR swapchain) noexcept;
		void destroy(VkSurfaceKHR surface) noexcept;

		/// Destroys every entry whose value has provably completed.
		void drain(const Context& ctx, u64 completed) noexcept;

		/// Entries not yet destroyed; telemetry and teardown asserts.
		[[nodiscard]] u32 pending() const noexcept { return m_entries.size() - m_head; }

	private:
		enum class Kind : u8
		{
			None = 0,
			Buffer,
			Image,
			ImageView,
			Semaphore,
			Swapchain,
			Surface,
		};

		struct Entry
		{
			u64 value				 = 0;			   // destroyed once the timeline passes this
			void* handle			 = 0;			   // the VK object's handle bits
			VmaAllocation allocation = VK_NULL_HANDLE; // Buffer & Image only.
			Kind kind				 = Kind::None;
		};

		// Non-dispatchable handles are distinct pointer types only on 64-bit platforms.
		static_assert(sizeof(void*) == 8, "Entry stores Vk handles as u64 pointer bits");
		static_assert(sizeof(Entry) == 32, "half a cache line; two entries per line");

		void enqueue(Entry dead) noexcept;

		Vector<Entry> m_entries{&memory::heap(MemoryTag::Graphics)};
		const FrameState* m_frame = nullptr;
		u32 m_head				  = 0;
		u32 m_owner				  = 0;
	};
}
