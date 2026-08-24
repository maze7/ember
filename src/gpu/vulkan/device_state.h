#pragma once

#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <ember/sync/thread.h>
#include <gpu/vulkan/common.h>
#include <gpu/vulkan/destroy_queue.h>
#include <gpu/vulkan/resources.h>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>

#include <atomic>

namespace ember
{
	class Platform;
}

namespace ember::gpu
{
	struct Queue
	{
		VkQueue handle = VK_NULL_HANDLE;
		u32 family	   = VK_QUEUE_FAMILY_IGNORED;
	};

	struct Context
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice adapter = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

		// Device Queues
		Queue graphics{}; // owns submission; also presents (checked at adapter selection)
		Queue compute{}; // dedicated async-compute family when the adapter has one, otherwise equals graphics
		Queue transfer{}; // dedicated DMA family when present, otherwise equals graphics

		DeviceCaps caps{};
		Platform* platform = nullptr;

		bool debug_utils = false;
	};

	struct FrameSlot
	{
		u64 submitted = 0; // timeline value that this slot's last end_frame signalled; 0 = never used.

		/// Whole-pool reset each frame (cheaper and more thorough than per-buffer reset).
		VkCommandPool pool		 = VK_NULL_HANDLE;
		VkCommandBuffer commands = VK_NULL_HANDLE; // one primary; more when recording parallelizes
	};

	/// Swapchains acquired this frame; end_frame clears, submits and presents them as a batch.
	struct PendingPresent
	{
		SwapchainHandle swapchain{};
		u32 image_index = 0;
	};

	/**
	 * Debug messenger state. Process-lifetime on purpose: the messenger reports during instance/device
	 * destruction, and tests assert zero messages *after* teardown. Inline function-local static, so
	 * no definition TU is needed.
	 */
	struct DebugState
	{
		std::atomic<u32> errors{0};
		std::atomic<u32> warnings{0};
		bool break_on_error = false;
	};

	[[nodiscard]] inline DebugState& debug_state() noexcept
	{
		static constinit DebugState s_debug{};
		return s_debug;
	}

	struct Backend
	{
		Context context;

		vk::DestroyQueue deferred{};
		vk::ResourcePools resources;

		VkPhysicalDeviceProperties properties{};
		VkPhysicalDeviceMemoryProperties memory_properties{};


		VkSemaphore timeline = VK_NULL_HANDLE; // the one CPU/GPU sync primitive: frame N signals N
		FrameSlot slots[MAX_FRAMES_IN_FLIGHT]{};

		u64 frame_index		 = 0;
		u32 frames_in_flight = 2;
		u64 timeline_value	 = 0;
		bool frame_open		 = false;

		std::array<PendingPresent, MAX_SWAPCHAINS> pending_presents{};
		u32 pending_present_count = 0;

		Backend() noexcept					   = default;
		Backend(const Backend&)			   = delete;
		Backend& operator=(const Backend&) = delete;

		/// Capabilities granted at boot that only the backend branches on.
		/// User-facing ones live in DeviceCaps.
		bool validation			   = false; // VK_LAYER_KHRONOS_validation actually enabled
		bool debug_utils		   = false; // object names + command labels available
		bool buffer_device_address = false; // enables VMA's BDA flag.
		bool memory_budget		   = false; // VK_EXT_memory_budget for VMA heap stats

		/// Bookkeeping
		u32 owner_thread = current_thread_id(); // the thread that constructed the Device
		std::atomic<bool> lost{false};			// sticky VK_ERROR_DEVICE_LOST
	};

#define EMBER_GPU_GUARD(...)                                                                                           \
	if (m_state == nullptr)                                                                                            \
		return __VA_ARGS__;                                                                                            \
	EMBER_ASSERT(m_state->owner_thread == current_thread_id())
}
