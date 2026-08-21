#pragma once

#include <ember/containers/pool.h>
#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <ember/memory/common.h>
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
		VkFormat format = VK_FORMAT_UNDEFINED;
		u32 mip_count	= 1;
		u32 layer_count = 1;

		// False for swapchain images.
		bool owns_image = true;
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

	struct SwapchainData
	{
		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		VkSurfaceKHR surface	 = VK_NULL_HANDLE;
		WindowHandle window{};
		VkFormat format				= VK_FORMAT_UNDEFINED;
		VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		VkExtent2D extent{};
		u32 acquired_image = 0;
		u32 image_count	   = 0;
		std::array<TextureHandle, MAX_SWAPCHAIN_IMAGES> images{};
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
			buffers.reserve(limits.max_buffers);
			textures.reserve(limits.max_textures);
			samplers.reserve(limits.max_samplers);
			graphics_pipelines.reserve(limits.max_graphics_pipelines);
			compute_pipelines.reserve(limits.max_compute_pipelines);
			swapchains.reserve(limits.max_swapchains);
		}
	};
}
