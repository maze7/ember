#pragma once

#include <ember/containers/pool.h>
#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <ember/gpu/swapchain.h>
#include <ember/memory/common.h>
#include <ember/memory/memory.h>
#include <ember/platform/window.h>
#include <gpu/vulkan/common.h>
#include <vk_mem_alloc.h>

#include <array>
#include <vulkan/vulkan_core.h>

namespace ember::gpu::vk
{
	struct BufferHot
	{
		VkBuffer handle			= VK_NULL_HANDLE;
		VkDeviceAddress address = 0;
	};

	struct BufferCold
	{
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkDeviceSize size		 = 0;
		void* mapped			 = nullptr; // non-null iff Upload/Readback (VMA persistent map)
	};

	struct TextureHot
	{
		VkImage image			 = VK_NULL_HANDLE;
		VkImageView sampled_view = VK_NULL_HANDLE;
		VkImageView storage_view = VK_NULL_HANDLE;
	};

	struct TextureCold
	{
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkExtent3D extent{};
		VkFormat format			 = VK_FORMAT_UNDEFINED;
		TextureFormat api_format = TextureFormat::Undefined;

		u32 mip_count	= 1;
		u32 layer_count = 1;

		/// Where the texture rests between uses
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

		// False for swapchain images.
		bool owns_image = true;

		TextureType type = TextureType::Texture2D;

		/// Internal subresource entries carry their parent here; user handles never do.
		TextureHandle parent{};

		/// Hidden per mip storage entries, mips 1 and deeper. Slot 0 of the chain is
		/// the texture's own bindless slot.
		TextureHandle mip_storage[MAX_MIP_LEVELS]{};

		/// Single slice attachment views, mip major, for render targets with more
		/// than one subresource. Empty otherwise; begin_rendering falls back to the
		/// whole view.
		Vector<VkImageView> attachment_views{&memory::heap(MemoryTag::Graphics)};
	};

	struct SamplerData
	{
		VkSampler handle = VK_NULL_HANDLE;
	};

	struct PipelineData
	{
		VkPipeline pipeline		= VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
	};

	/// Sentinel for SwapchainData::acquired_image: nothing acquired.
	inline constexpr u32 NO_IMAGE = ~0u;

	/**
	 * One swapchain: the native objects, its WSI semaphores and its backbuffer pool entries.
	 *
	 * Backbuffers live in the texture pool with owns_image = false, so acquire() returns a
	 * plain TextureHandle and later render-pass code needs no special cases.
	 */
	struct SwapchainData
	{
		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		VkSurfaceKHR surface	 = VK_NULL_HANDLE;
		WindowHandle window{};

		VkSurfaceFormatKHR surface_format{};
		VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
		PresentMode requested_mode	  = PresentMode::VSync; // re-resolved on every recreate
		u32 preferred_image_count	  = 3;

		VkExtent2D extent{};
		u32 image_count = 0;
		std::array<TextureHandle, MAX_SWAPCHAIN_IMAGES> images{};

		/// Same-frame acquire cache: a second acquire() in one frame returns the same image.
		u32 acquired_image = NO_IMAGE;
		u64 acquired_frame = 0; // frame_index + 1 of the acquiring frame; 0 = never

		/// One per frame slot. Reuse is safe because begin_frame's timeline wait proves the
		/// submit that consumed the semaphore has retired.
		std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> acquire_semaphores{};

		/// One per swapchain image, indexed by acquired image index. Reuse is safe because
		/// re-acquiring image i implies the presentation engine consumed its previous wait.
		std::array<VkSemaphore, MAX_SWAPCHAIN_IMAGES> present_semaphores{};

		bool needs_recreate = false; // set by present results / SUBOPTIMAL at acquire
	};

	struct ResourcePools final
	{
		Pool<Buffer, BufferHot, BufferCold> buffers{MemoryTag::Graphics};
		Pool<Texture, TextureHot, TextureCold> textures{MemoryTag::Graphics};
		Pool<Sampler, SamplerData> samplers{MemoryTag::Graphics};
		Pool<GraphicsPipeline, PipelineData> graphics_pipelines{MemoryTag::Graphics};
		Pool<ComputePipeline, PipelineData> compute_pipelines{MemoryTag::Graphics};
		Pool<Swapchain, SwapchainData> swapchains{MemoryTag::Graphics};

		void reserve(const DeviceLimits& limits) noexcept
		{
			buffers.init(limits.max_buffers);
			textures.init(limits.max_textures);
			samplers.init(limits.max_samplers);
			graphics_pipelines.init(limits.max_graphics_pipelines);
			compute_pipelines.init(limits.max_compute_pipelines);
			swapchains.init(limits.max_swapchains);
		}
	};
}
