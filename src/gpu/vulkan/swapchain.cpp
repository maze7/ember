#include <gpu/vulkan/swapchain.h>

#include <ember/core/profile.h>
#include <ember/gpu/common.h>
#include <ember/platform/platform.h>
#include <platform/vulkan/wsi.h>

#include <algorithm>

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
			constexpr u32 MAX_FORMATS = 64;

			u32 count = 0;
			vkGetPhysicalDeviceSurfaceFormatsKHR(adapter, surface, &count, nullptr);

			VkSurfaceFormatKHR formats[MAX_FORMATS];
			count = std::min(count, MAX_FORMATS);
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

		/// Requested mode when the surface offers it; FIFO otherwise (the only guaranteed mode).
		[[nodiscard]] VkPresentModeKHR
		choose_present_mode(VkPhysicalDevice adapter, VkSurfaceKHR surface, PresentMode requested) noexcept
		{
			if (requested == PresentMode::VSync)
				return VK_PRESENT_MODE_FIFO_KHR;

			const VkPresentModeKHR wanted =
				requested == PresentMode::Mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;

			constexpr u32 MAX_MODES = 8;

			u32 count = 0;
			vkGetPhysicalDeviceSurfacePresentModesKHR(adapter, surface, &count, nullptr);

			VkPresentModeKHR modes[MAX_MODES];
			count = std::min(count, MAX_MODES);
			vkGetPhysicalDeviceSurfacePresentModesKHR(adapter, surface, &count, modes);

			for (u32 i = 0; i < count; ++i)
				if (modes[i] == wanted)
					return wanted;

			EMBER_INFO("vulkan: requested present mode unavailable, using VSync");
			return VK_PRESENT_MODE_FIFO_KHR;
		}

		/// Log-friendly name for the modes this layer can select.
		[[nodiscard]] const char* present_mode_name(VkPresentModeKHR mode) noexcept
		{
			switch (mode)
			{
				case VK_PRESENT_MODE_MAILBOX_KHR:
					return "mailbox";
				case VK_PRESENT_MODE_IMMEDIATE_KHR:
					return "immediate";
				default:
					return "vsync";
			}
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
			const DeviceState& backend, const SwapchainData& data, const VkSurfaceCapabilitiesKHR& caps) noexcept
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

		/**
		 * Retires every per-image object of the current VkSwapchainKHR: the pool handles die now,
		 * the native views and present semaphores die through the destroy queue when the frames
		 * that could touch them retire. The swapchain object itself is the caller's to defer.
		 */
		void retire_backbuffers(DeviceState& backend, SwapchainData& data) noexcept
		{
			for (u32 i = 0; i < data.image_count; ++i)
			{
				if (const TextureHot* hot = backend.resources.textures.try_get(data.images[i]))
					defer_destroy(backend, hot->sampled_view);

				defer_destroy(backend, data.present_semaphores[i]);
				data.present_semaphores[i] = VK_NULL_HANDLE;

				(void)backend.resources.textures.erase(data.images[i]);
				data.images[i] = {};
			}

			data.image_count = 0;
		}

		enum class Build : u8
		{
			Ok,
			Suspended, // zero extent (minimized): nothing (re)built, any existing swapchain kept
			Failed,
		};

		/**
		 * (Re)creates the VkSwapchainKHR and its backbuffer pool entries. The old swapchain is
		 * passed as oldSwapchain so the driver can recycle buffers across resizes; its retired
		 * objects then age out through the destroy queue.
		 */
		[[nodiscard]] Build build(DeviceState& backend, SwapchainData& data) noexcept
		{
			EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);

			VkSurfaceCapabilitiesKHR surface_caps{};
			if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(backend.adapter, data.surface, &surface_caps) != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: surface capability query failed");
				return Build::Failed;
			}

			const VkExtent2D extent = resolve_extent(backend, data, surface_caps);

			// Minimized: never create a zero-extent swapchain. Any existing one stays; it is
			// still valid after restore, or the next acquire's OUT_OF_DATE rebuilds it.
			if (extent.width == 0 || extent.height == 0)
				return Build::Suspended;

			// maxImageCount == 0 means unbounded; the engine max keeps per-image arrays indexable.
			u32 image_count = std::max(data.preferred_image_count, surface_caps.minImageCount);
			image_count		= std::min(image_count, MAX_SWAPCHAIN_IMAGES);
			if (surface_caps.maxImageCount != 0)
				image_count = std::min(image_count, surface_caps.maxImageCount);

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
				return Build::Failed;
			}

			// Old generation retires when the frames that could touch it do; the driver
			// already got its recycling shot via oldSwapchain above.
			retire_backbuffers(backend, data);

			if (old != VK_NULL_HANDLE)
				defer_destroy(backend, old);

			data.swapchain = swapchain;
			data.extent	   = extent;

			// Wrap the backbuffers as externally-owned texture pool entries.
			VkImage images[MAX_SWAPCHAIN_IMAGES];
			u32 count = 0;
			EMBER_VK_CHECK(vkGetSwapchainImagesKHR(backend.device, data.swapchain, &count, nullptr));

			// acquire() indexes per-image state (present semaphores, backbuffer handles) by the driver's
			// image index, which ranges over the full image array. A swapchain we cannot track in full is
			// unusable, so this is a hard failure. Never acquired from, so immediate destroy is legal.
			if (count > MAX_SWAPCHAIN_IMAGES)
			{
				EMBER_ERROR("vulkan: swapchain has {} images, ember supports {}", count, MAX_SWAPCHAIN_IMAGES);
				vkDestroySwapchainKHR(backend.device, data.swapchain, nullptr);
				data.swapchain = VK_NULL_HANDLE; // consistent "no swapchain"; next acquire retries a full build
				return Build::Failed;
			}

			EMBER_VK_CHECK(vkGetSwapchainImagesKHR(backend.device, data.swapchain, &count, images));

			const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

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
				EMBER_VK_CHECK(
					vkCreateSemaphore(backend.device, &semaphore_info, nullptr, &data.present_semaphores[i]));

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

			EMBER_INFO(
				"vulkan: swapchain {}x{} x{} images | {}",
				extent.width,
				extent.height,
				count,
				present_mode_name(data.present_mode));

			return Build::Ok;
		}
	}

	bool swapchain_create(DeviceState& backend, const SwapchainDef& def, SwapchainData& data) noexcept
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

		// Suspended (created while minimized) is fine: the first acquire after restore builds.
		if (build(backend, data) == Build::Failed)
		{
			swapchain_destroy(backend, data);
			return false;
		}

		// Per-slot acquire semaphores, created once: frames_in_flight is fixed at boot, so a
		// recreate never needs more.
		const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

		for (u32 i = 0; i < backend.frames_in_flight; ++i)
			EMBER_VK_CHECK(vkCreateSemaphore(backend.device, &semaphore_info, nullptr, &data.acquire_semaphores[i]));

		return true;
	}

	void swapchain_destroy(DeviceState& backend, SwapchainData& data) noexcept
	{
		retire_backbuffers(backend, data);

		for (VkSemaphore& semaphore : data.acquire_semaphores)
		{
			defer_destroy(backend, semaphore);
			semaphore = VK_NULL_HANDLE;
		}

		defer_destroy(backend, data.swapchain);
		data.swapchain = VK_NULL_HANDLE;

		defer_destroy(backend, data.surface);
		data.surface = VK_NULL_HANDLE;
	}

	TextureHandle swapchain_acquire(DeviceState& backend, SwapchainHandle handle) noexcept
	{
		SwapchainData& data = backend.resources.swapchains.get(handle);

		// Same-frame cache: only the first acquire per frame pays for anything.
		const u64 frame_token = backend.frame_index + 1;
		if (data.acquired_frame == frame_token)
			return data.images[data.acquired_image];

		// Minimized: report suspended without touching the swapchain; it revives on restore.
		const glm::uvec2 pixels = backend.platform->window_pixel_size(data.window);
		if (pixels.x == 0 || pixels.y == 0)
			return {};

		const bool extent_changed = pixels.x != data.extent.width || pixels.y != data.extent.height;

		if (data.needs_recreate || extent_changed || data.swapchain == VK_NULL_HANDLE)
		{
			if (build(backend, data) != Build::Ok)
				return {};

			data.needs_recreate = false;
		}

		const u32 slot = static_cast<u32>(backend.frame_index % backend.frames_in_flight);

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
				if (build(backend, data) != Build::Ok)
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
