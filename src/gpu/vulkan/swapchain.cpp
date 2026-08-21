#include "ember/gpu/common.h"
#include <gpu/vulkan/swapchain.h>

#include <ember/core/profile.h>
#include <ember/platform/platform.h>
#include <platform/vulkan/wsi.h>

#include <algorithm>
#include <vulkan/vulkan_core.h>

namespace ember::gpu::vk
{
	namespace
	{
		/**
		 * Prefer 8-bit sRGB, BGRA first (the native order on every desktop compositor), then
		 * UNORM fallbacks. HDR formats are a deliberate later extension.
		 */
		[[nodiscard]] VkSurfaceFormatKHR choose_surface_format(VkPhysicalDevice adapter, VkSurfaceKHR surface) noexcept
		{
			u32 count = 0;
			vkGetPhysicalDeviceSurfaceFormatsKHR(adapter, surface, &count, nullptr);

			VkSurfaceFormatKHR formats[64];
			count = std::min(count, 64u);
			vkGetPhysicalDeviceSurfaceFormatsKHR(adapter, surface, &count, formats);

			constexpr VkFormat PREFERRED[] = {
				VK_FORMAT_B8G8R8A8_SRGB,
				VK_FORMAT_R8G8B8A8_SRGB,
				VK_FORMAT_B8G8R8A8_UNORM,
				VK_FORMAT_R8G8B8A8_UNORM,
			};

			for (VkFormat wanted : PREFERRED)
			{
				for (u32 i = 0; i < count; ++i)
				{
					if (formats[i].format == wanted && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
						return formats[i];
				}
			}

			EMBER_WARN("vulkan: no preferred swapchain format; using the surface's first");
			return count > 0 ? formats[0]
							 : VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
		}

		/// Requested mode when the surface offsets it; FIFO otherwise (the only guaranteed mode).
		[[nodiscard]] VkPresentModeKHR
		choose_present_mode(VkPhysicalDevice adapter, VkSurfaceKHR surface, PresentMode requested) noexcept
		{
			if (requested == PresentMode::VSync)
				return VK_PRESENT_MODE_FIFO_KHR;

			VkPresentModeKHR wanted =
				requested == PresentMode::Mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;

			// Retrieve the number of present modes
			u32 count = 0;
			vkGetPhysicalDeviceSurfacePresentModesKHR(adapter, surface, &count, nullptr);

			// Retrieve the actual present modes
			VkPresentModeKHR modes[8];
			count = std::min(count, 8u);
			vkGetPhysicalDeviceSurfacePresentModesKHR(adapter, surface, &count, modes);

			for (u32 i = 0; i < count; ++i)
				if (modes[i] == wanted)
					return wanted;

			EMBER_INFO("vulkan: requested present mode unavailable, using VSync");
			return VK_PRESENT_MODE_FIFO_KHR;
		}

		/// Wayland compositors may only offer PRE_MULTIPLIED; never hardcode OPAQUE.
		[[nodiscard]] VkCompositeAlphaFlagBitsKHR choose_composite_alpha(const VkSurfaceCapabilitiesKHR& caps) noexcept
		{
			constexpr VkCompositeAlphaFlagBitsKHR PREFERRED[] = {
				VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
				VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
				VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
				VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
			};

			for (VkCompositeAlphaFlagBitsKHR alpha : PREFERRED)
				if ((caps.supportedCompositeAlpha & alpha) != 0)
					return alpha;

			return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		}

		/// The window's pixel size, clamped to surface limits. {0,0} = minimized (suspend).
		[[nodiscard]] VkExtent2D resolve_extent(
			const DeviceBackend& backend, const SwapchainData& data, const VkSurfaceCapabilitiesKHR& caps) noexcept
		{
			// 0xFFFFFFFF means "the surface follows the swapchain" (Wayland); the window is
			// the authority then. Otherwise the surface dictates exactly.
			if (caps.currentExtent.width != ~0u)
				return caps.currentExtent;

			const glm::uvec2 pixels = backend.platform->window_pixel_size(data.window);

			return {
				std::clamp(pixels.x, caps.minImageExtent.width, caps.maxImageExtent.width),
				std::clamp(pixels.y, caps.minImageExtent.height, caps.maxImageExtent.height),
			};
		}

		void destroy_backbuffer_views(DeviceBackend& backend, SwapchainData& data) noexcept
		{
			for (u32 i = 0; i < data.image_count; ++i)
			{
				if (data.images[i].is_null())
					continue;

				if (const TextureHot* hot = backend.resources.textures.try_get(data.images[i]))
					vkDestroyImageView(backend.device, hot->sampled_view, nullptr);

				(void)backend.resources.textures.erase(data.images[i]);
				data.images[i] = {};
			}

			data.image_count = 0;
		}

		/**
		 * (Re)creates the VkSwapchainKHR and its backbuffer pool entries. The old swapchain is
		 * passed as oldSwapchain so the driver can recycle buffers across resizes.
		 *
		 * Deliberate simplification for this slice: callers wait-idle before retiring old
		 * objects. The deferred-deletion slice replaces that wait with per-slot retirement.
		 */
		[[nodiscard]] bool build(DeviceBackend& backend, SwapchainData& data) noexcept
		{
			EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);

			VkSurfaceCapabilitiesKHR surface_caps{};
			if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(backend.adapter, data.surface, &surface_caps) != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: surface capability query failed");
				return false;
			}

			const VkExtent2D extent = resolve_extent(backend, data, surface_caps);

			// Minimized: never create a zero-extent swapchain; acquire() reports suspended.
			if (extent.width == 0 || extent.height == 0)
			{
				data.suspended = true;
				return true;
			}

			// maxImageCount == 0 means unbounded.
			u32 image_count = std::max(data.preferred_image_count, surface_caps.minImageCount);
			if (surface_caps.maxImageCount != 0)
				image_count = std::min(image_count, surface_caps.maxImageCount);

			// Hard cap, not truncation: present semaphores are indexed by image index.
			EMBER_ASSERT(image_count <= MAX_SWAPCHAIN_IMAGES);

			data.surface_format = choose_surface_format(backend.adapter, data.surface);
			data.present_mode	= choose_present_mode(backend.adapter, data.surface, data.requested_mode);

			// TRANSFER_SRC when offered: screenshots/readback later, free to request now.
			VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if ((surface_caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0)
				usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

			const VkSwapchainKHR old = data.swapchain;

			const VkSwapchainCreateInfoKHR info{
				.sType			  = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
				.surface		  = data.surface,
				.minImageCount	  = image_count,
				.imageFormat	  = data.surface_format.format,
				.imageColorSpace  = data.surface_format.colorSpace,
				.imageExtent	  = extent,
				.imageArrayLayers = 1,
				.imageUsage		  = usage,
				.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, // graphics == present, enforced at boot
				.preTransform	  = surface_caps.currentTransform,
				.compositeAlpha	  = choose_composite_alpha(surface_caps),
				.presentMode	  = data.present_mode,
				.clipped		  = VK_TRUE,
				.oldSwapchain	  = old,
			};

			VkSwapchainKHR swapchain = VK_NULL_HANDLE;
			if (const VkResult result = vkCreateSwapchainKHR(backend.device, &info, nullptr, &swapchain);
				result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vkCreateSwapchainKHR failed: {}", result_name(result));
				return false;
			}

			// Old generation retires now (callers guaranteed idle this slice).
			destroy_backbuffer_views(backend, data);
			if (old != VK_NULL_HANDLE)
				vkDestroySwapchainKHR(backend.device, old, nullptr);

			data.swapchain = swapchain;
			data.extent	   = extent;
			data.suspended = false;

			// Wrap the backbuffers as externally-owned texture pool entries.
			VkImage images[MAX_SWAPCHAIN_IMAGES];
			u32 count = 0;
			vkGetSwapchainImagesKHR(backend.device, data.swapchain, &count, nullptr);
			EMBER_ASSERT(count <= MAX_SWAPCHAIN_IMAGES);
			vkGetSwapchainImagesKHR(backend.device, data.swapchain, &count, images);

			for (u32 i = 0; i < count; ++i)
			{
				const VkImageViewCreateInfo view_info{
					.sType			  = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					.image			  = images[i],
					.viewType		  = VK_IMAGE_VIEW_TYPE_2D,
					.format			  = data.surface_format.format,
					.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};

				VkImageView view = VK_NULL_HANDLE;
				EMBER_VK_CHECK(vkCreateImageView(backend.device, &view_info, nullptr, &view));

				data.images[i] = backend.resources.textures.insert(
					TextureHot{.image = images[i], .sampled_view = view},
					TextureCold{
						.extent		= {extent.width, extent.height, 1},
						.format		= data.surface_format.format,
						.owns_image = false, // the swapchain owns these; destroy() must skip them
					});

				set_name(backend, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(images[i]), "ember.backbuffer");
			}

			data.image_count = count;
			return true;
		}
	}

	bool swapchain_create(DeviceBackend& backend, const SwapchainDef& def, SwapchainData& data) noexcept
	{
		data.window				   = def.window;
		data.requested_mode		   = def.present_mode;
		data.preferred_image_count = def.image_count;

		// Surface first: it outlives every swapchain generation for this window.
		data.surface = platform::vk::create_surface(backend.platform->native_window(def.window), backend.instance);

		if (data.surface == VK_NULL_HANDLE)
		{
			EMBER_ERROR("vulkan: surface creation failed");
			return false;
		}

		if (!build(backend, data))
		{
			swapchain_destroy(backend, data);
			return false;
		}

		// Per-slot acquire + per-image present semaphores. Created for the compile-time image
		// maximum so a recreate that grows image_count never needs new semaphores.
		VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

		for (u32 i = 0; i < backend.frames_in_flight; ++i)
			EMBER_VK_CHECK(vkCreateSemaphore(backend.device, &semaphore_info, nullptr, &data.acquire_semaphores[i]));

		for (u32 i = 0; i < MAX_SWAPCHAIN_IMAGES; ++i)
			EMBER_VK_CHECK(vkCreateSemaphore(backend.device, &semaphore_info, nullptr, &data.present_semaphores[i]));

		EMBER_INFO(
			"vulkan: swapchain {}x{} x{} images | {}",
			data.extent.width,
			data.extent.height,
			data.image_count,
			data.present_mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : "unlocked");

		return true;
	}

	void swapchain_destroy(DeviceBackend& backend, SwapchainData& data) noexcept
	{
		// Cold path (teardown / explicit destroy): idling beats lifetime machinery here.
		(void)vkDeviceWaitIdle(backend.device);

		destroy_backbuffer_views(backend, data);

		for (VkSemaphore& semaphore : data.acquire_semaphores)
		{
			if (semaphore != VK_NULL_HANDLE)
				vkDestroySemaphore(backend.device, semaphore, nullptr);
			semaphore = VK_NULL_HANDLE;
		}

		for (VkSemaphore& semaphore : data.present_semaphores)
		{
			if (semaphore != VK_NULL_HANDLE)
				vkDestroySemaphore(backend.device, semaphore, nullptr);
			semaphore = VK_NULL_HANDLE;
		}

		if (data.swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(backend.device, data.swapchain, nullptr);

		if (data.surface != VK_NULL_HANDLE)
			platform::vk::destroy_surface(backend.instance, data.surface);

		data.swapchain = VK_NULL_HANDLE;
		data.surface   = VK_NULL_HANDLE;
	}

	TextureHandle swapchain_acquire(DeviceBackend& backend, SwapchainHandle handle, u32 slot) noexcept
	{
		SwapchainData& data = backend.resources.swapchains.get(handle);

		// Same-frame cache: only the first acquire per frame pays for anything.
		const u64 frame_token = backend.frame_index + 1;
		if (data.acquired_frame == frame_token)
			return data.images[data.acquired_image];

		const glm::uvec2 pixels = backend.platform->window_pixel_size(data.window);

		if (pixels.x == 0 || pixels.y == 0)
		{
			data.suspended = true;
			return {};
		}

		const bool extent_changed = pixels.x != data.extent.width || pixels.y != data.extent.height;

		if (data.suspended || data.needs_recreate || extent_changed || data.swapchain == VK_NULL_HANDLE)
		{
			// Simplification (see build()): idle before retiring the old generation.
			(void)vkDeviceWaitIdle(backend.device);

			if (!build(backend, data))
				return {};

			data.needs_recreate = false;

			if (data.suspended)
				return {};
		}

		// OUT_OF_DATE signals nothing and leaves the semaphore unsignaled, so retrying with the
		// same semaphore is safe. Bounded: one recreate attempt, then give up this frame.
		for (u32 attempt = 0; attempt < 2; ++attempt)
		{
			u32 image_index		  = NO_IMAGE;
			const VkResult result = vkAcquireNextImageKHR(
				backend.device,
				data.swapchain,
				UINT64_MAX,
				data.acquire_semaphores[slot],
				VK_NULL_HANDLE,
				&image_index);

			if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
			{
				// SUBOPTIMAL still signaled: render and present this frame, recreate next.
				data.needs_recreate |= result == VK_SUBOPTIMAL_KHR;
				data.acquired_image	 = image_index;
				data.acquired_frame	 = frame_token;

				EMBER_ASSERT(backend.pending_present_count < MAX_SWAPCHAINS);
				backend.pending_presents[backend.pending_present_count++] = {
					.swapchain	 = handle,
					.image_index = image_index,
				};

				return data.images[image_index];
			}

			if (result == VK_ERROR_OUT_OF_DATE_KHR)
			{
				(void)vkDeviceWaitIdle(backend.device);

				if (!build(backend, data) || data.suspended)
					return {};

				continue;
			}

			note_result(backend, result);
			EMBER_ERROR("vulkan: vkAcquireNextImageKHR failed: {}", result_name(result));
			return {};
		}

		return {};
	}
}
