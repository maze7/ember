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

namespace ember::gpu::vk
{
	struct Queue
	{
		VkQueue handle = VK_NULL_HANDLE;
		u32 family	   = VK_QUEUE_FAMILY_IGNORED;
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

	struct DeviceBackend
	{
		/// Boot state. Written by boot(), constant afterwards.
		VkInstance instance				   = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
		VkPhysicalDevice adapter		   = VK_NULL_HANDLE;
		VkDevice device					   = VK_NULL_HANDLE;
		VmaAllocator allocator			   = VK_NULL_HANDLE;
		VkPipelineCache pipeline_cache	   = VK_NULL_HANDLE;

		Queue graphics{}; // owns submission; also presents (checked at adapter selection)
		Queue compute{};  // dedicated async-compute family when the adapter has one, else == graphics
		Queue transfer{}; // dedicated DMA family when present, else == graphics

		DestroyQueue deferred{};

		VkPhysicalDeviceProperties properties{};
		VkPhysicalDeviceMemoryProperties memory_properties{};
		DeviceCaps caps{};

		Platform* platform = nullptr; // Window-system provider bound at boot; null == headless.

		ResourcePools resources;

		VkSemaphore timeline = VK_NULL_HANDLE; // the one CPU/GPU sync primitive: frame N signals N
		FrameSlot slots[MAX_FRAMES_IN_FLIGHT]{};

		u64 frame_index		 = 0;
		u32 frames_in_flight = 2;
		u64 timeline_value	 = 0;
		bool frame_open		 = false;

		std::array<PendingPresent, MAX_SWAPCHAINS> pending_presents{};
		u32 pending_present_count = 0;

		DeviceBackend() noexcept					   = default;
		DeviceBackend(const DeviceBackend&)			   = delete;
		DeviceBackend& operator=(const DeviceBackend&) = delete;

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

	/// Full backend boot. Loader -> instance -> adapter -> device -> allocator -> PSO cache -> pools -> timeline
	[[nodiscard]] bool boot(DeviceBackend& backend, const DeviceDef& def) noexcept;

	/// Reverse of boot(), in struct reverse dependency order. Idempotent.
	void shutdown(DeviceBackend& backend) noexcept;

	/// Sets the debug name for a given vulkan object. Useful for viewing in RenderDoc.
	inline void set_name(const DeviceBackend& backend, VkObjectType type, u64 handle, const char* name) noexcept
	{
		if (backend.device == VK_NULL_HANDLE || name == nullptr)
			return;

		VkDebugUtilsObjectNameInfoEXT info{
			.sType		  = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.pNext		  = nullptr,
			.objectType	  = type,
			.objectHandle = handle,
			.pObjectName  = name,
		};

		(void)vkSetDebugUtilsObjectNameEXT(backend.device, &info);
	}

	/// Device loss is stick: every later call no-ops and the app is expected to tear down.
	inline void note_result(DeviceBackend& backend, VkResult result) noexcept
	{
		if (result == VK_ERROR_DEVICE_LOST) [[unlikely]]
		{
			if (!backend.lost.exchange(true, std::memory_order_acq_rel))
				EMBER_ERROR("vulkan: device lost (TDR or driver fault); the Device must be recreated");
		}
	}
}
