#pragma once

#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <ember/gpu/transient.h>
#include <ember/sync/thread.h>
#include <gpu/vulkan/common.h>
#include <gpu/vulkan/descriptor_heap.h>
#include <gpu/vulkan/destroy_queue.h>
#include <gpu/vulkan/resources.h>
#include <gpu/vulkan/staging.h>
#include <gpu/vulkan/transient_ring.h>

#include <vk_mem_alloc.h>

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

	/**
	 * Everything created exactly once at boot and read-only afterwards.
	 *
	 * Only device_boot.cpp writes these fields; everywhere else reads them. A field
	 * earns its place here with a post-boot reader or a destroy obligation; boot-only
	 * facts live in AdapterInfo or locals inside device_boot.cpp.
	 */
	struct Context
	{
		VkInstance instance				   = VK_NULL_HANDLE;
		VkPhysicalDevice adapter		   = VK_NULL_HANDLE;
		VkDevice device					   = VK_NULL_HANDLE;
		VmaAllocator allocator			   = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

		// Device Queues
		Queue graphics{}; // owns submission; also presents (checked at adapter selection)
		Queue compute{};  // dedicated async-compute family when the adapter has one, otherwise equals graphics
		Queue transfer{}; // dedicated DMA family when present, otherwise equals graphics

		DeviceCaps caps{};
		Platform* platform = nullptr;

		u32 frames_in_flight = 0; // clamped to [1, MAX_FRAMES_IN_FLIGHT] by boot()

		bool debug_utils = false; // set_name / pass labels are callable (extension enabled)

		/// Every enabled stage that can read resources. Sync2 masks may only name
		/// enabled stages, so task/mesh join only when mesh shaders booted.
		VkPipelineStageFlags2 all_shader_stages = 0;
	};

	/**
	 * Everything one CommandList records through: the native buffer and the shadow state
	 * the lazy flushes compare against. Single writer from begin to submit.
	 * Lives in fixed backend storage; an open list holds a pointer into it.
	 */
	struct Recording
	{
		VkCommandBuffer commands = VK_NULL_HANDLE;

		/// Redundancy filters: rebinding the bound pipeline is free to skip.
		GraphicsPipelineHandle graphics_pipeline{};
		ComputePipelineHandle compute_pipeline{};

		VkBuffer index_buffer  = VK_NULL_HANDLE;
		u64 index_offset	   = 0;
		VkIndexType index_type = VK_INDEX_TYPE_UINT32;

		u32 constant_offsets[CONSTANT_BUFFER_SLOTS]{};

		/// Constants serve both bind points; each flushes on its own schedule.
		bool constants_dirty_graphics = false;
		bool constants_dirty_compute  = false;
		bool index_dirty			  = false;
		bool inside_pass			  = false;

		/// Zones recorded by this list: names and colors stay CPU side, the query pool
		/// holds only the ticks. Resolved by the next begin_frame on this slot.
		struct Zone
		{
			const char* name = nullptr;
			u32 color		 = 0;
			u8 depth		 = 0;
		};

		Zone zones[MAX_GPU_ZONES]{};
		u32 zone_count = 0;
		u8 zone_depth  = 0;
	};

	struct FrameSlot
	{
		u64 submitted = 0; // timeline value that this slot's last end_frame signalled; 0 = never used.

		/// Whole-pool reset each frame (cheaper and more thorough than per-buffer reset).
		VkCommandPool pool = VK_NULL_HANDLE;
		Recording recording{}; // one open list per frame; an array once recording parallelizes
	};

	/// Swapchains acquired this frame; end_frame clears, submits and presents them as a batch.
	struct PendingPresent
	{
		SwapchainHandle swapchain{};
		u32 image_index = 0;
	};

	/// The frame loop's world: everything on the begin/end_frame clock. Owner thread only.
	struct FrameState
	{
		VkSemaphore timeline   = VK_NULL_HANDLE; // frame N signals value N
		VkQueryPool timestamps = VK_NULL_HANDLE; // zone ticks, sliced per slot; null without caps.timestamps
		u64 timeline_value	 = 0;			   // last value handed to a submit
		u64 index			 = 0;			   // slot = index % frames_in_flight
		bool open			 = false;

		bool list_open		= false;
		u32 lists_submitted = 0;

		/// Highest timeline value proven complete (begin_frame waits, wait_idle). Batch and
		/// page recycling key off this instead of querying the semaphore.
		u64 completed = 0;

		FrameSlot slots[MAX_FRAMES_IN_FLIGHT]{};
		PendingPresent pending_presents[MAX_SWAPCHAINS]{};
		u32 pending_present_count = 0;
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

	/**
	 * The compiled-in backend's entire state — what Device::m_state points at.
	 * Three zones, three mutation clocks:
	 *
	 *   context  — written by boot, read-only afterwards
	 *   frame    — written by the frame loop (begin/end_frame, acquire)
	 *   services — written by user calls (create/destroy), drained by the frame loop
	 */
	struct Backend
	{
		Context context;
		FrameState frame;

		// Services.
		vk::DestroyQueue destroy_queue{};
		vk::ResourcePools resources{};
		vk::DescriptorHeap descriptor_heap{}; // bindless heap
		TransientAllocator transient{};		  // fast path; user-facing via Device::transient()
		vk::TransientRing transient_ring{};	  // its memory, overflow pages, telemetry
		vk::Staging staging{};				  // staging ring + upload batches

		/// Zones from the most recently retired frame, refreshed by begin_frame.
		GpuZoneTiming gpu_zones[MAX_GPU_ZONES]{};
		u32 gpu_zone_count = 0;

		/// Bookkeeping
		u32 owner_thread = current_thread_id(); // the thread that constructed the Device
		std::atomic<bool> lost{false};			// sticky VK_ERROR_DEVICE_LOST

		Backend() noexcept				   = default;
		Backend(const Backend&)			   = delete;
		Backend& operator=(const Backend&) = delete;
	};

	namespace vk
	{
		/// Boot orchestrator (device_boot.cpp): fills context and frame; false on failure.
		/// Partial progress is legal: destroy_boot_state tears down whatever exists.
		[[nodiscard]] bool boot(Device& device, Backend& backend, const DeviceDef& def) noexcept;

		/// Reverse of boot, tolerant of partial boots. Resource pools must already be
		/// drained and the GPU idle. Leaves the Backend allocation itself alive.
		void destroy_boot_state(Backend& backend) noexcept;
	}

#define EMBER_GPU_GUARD(...)                                                                                           \
	if (m_backend == nullptr)                                                                                          \
		return __VA_ARGS__;                                                                                            \
	EMBER_ASSERT(m_backend->owner_thread == current_thread_id())
}
