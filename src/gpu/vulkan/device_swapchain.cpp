#include <ember/core/logger.h>
#include <ember/core/profile.h>
#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <ember/platform/platform.h>
#include <gpu/vulkan/backend.h>
#include <platform/vulkan/wsi.h>

#include <algorithm>

namespace ember::gpu
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
			const Platform& platform, const vk::SwapchainData& data, const VkSurfaceCapabilitiesKHR& caps) noexcept
		{
			// 0xFFFFFFFF means "the surface follows the swapchain" (Wayland); the window is
			// the authority then. Otherwise the surface dictates exactly.
			if (caps.currentExtent.width != ~0u)
				return caps.currentExtent;

			const glm::uvec2 pixels = platform.window_pixel_size(data.window);

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
		void retire_backbuffers(Backend& backend, vk::SwapchainData& data) noexcept
		{
			for (u32 i = 0; i < data.image_count; ++i)
			{
				if (const vk::TextureHot* hot = backend.resources.textures.get(data.images[i]))
					backend.destroy_queue.destroy(hot->sampled_view);

				backend.destroy_queue.destroy(data.present_semaphores[i]);
				data.present_semaphores[i] = VK_NULL_HANDLE;

				(void)backend.resources.textures.erase(data.images[i]);
				data.images[i] = {};
			}

			data.image_count = 0;
		}

		void destroy_swapchain_data(Backend& backend, vk::SwapchainData& data) noexcept
		{
			retire_backbuffers(backend, data);

			for (auto& semaphore : data.acquire_semaphores)
			{
				backend.destroy_queue.destroy(semaphore);

				semaphore = VK_NULL_HANDLE;
			}

			backend.destroy_queue.destroy(data.swapchain);
			backend.destroy_queue.destroy(data.surface);

			data.swapchain = VK_NULL_HANDLE;
			data.surface   = VK_NULL_HANDLE;
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
		[[nodiscard]] Build build(Backend& backend, vk::SwapchainData& data) noexcept
		{
			EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);

			VkSurfaceCapabilitiesKHR surface_caps{};
			if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(backend.context.adapter, data.surface, &surface_caps) !=
				VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: surface capability query failed");
				return Build::Failed;
			}

			const VkExtent2D extent = resolve_extent(*backend.context.platform, data, surface_caps);

			// Minimized: never create a zero-extent swapchain. Any existing one stays; it is
			// still valid after restore, or the next acquire's OUT_OF_DATE rebuilds it.
			if (extent.width == 0 || extent.height == 0)
				return Build::Suspended;

			// maxImageCount == 0 means unbounded; the engine max keeps per-image arrays indexable.
			u32 image_count = std::max(data.preferred_image_count, surface_caps.minImageCount);
			image_count		= std::min(image_count, MAX_SWAPCHAIN_IMAGES);
			if (surface_caps.maxImageCount != 0)
				image_count = std::min(image_count, surface_caps.maxImageCount);

			data.surface_format = choose_surface_format(backend.context.adapter, data.surface);
			data.present_mode	= choose_present_mode(backend.context.adapter, data.surface, data.requested_mode);

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
			if (const VkResult result = vkCreateSwapchainKHR(backend.context.device, &info, nullptr, &swapchain);
				result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vkCreateSwapchainKHR failed: {}", vk::result_name(result));
				return Build::Failed;
			}

			// Old generation retires when the frames that could touch it do; the driver
			// already got its recycling shot via oldSwapchain above.
			retire_backbuffers(backend, data);

			if (old != VK_NULL_HANDLE)
				backend.destroy_queue.destroy(old);

			data.swapchain = swapchain;
			data.extent	   = extent;

			// Wrap the backbuffers as externally-owned texture pool entries.
			VkImage images[MAX_SWAPCHAIN_IMAGES];
			u32 count = 0;
			EMBER_VK_CHECK(vkGetSwapchainImagesKHR(backend.context.device, data.swapchain, &count, nullptr));

			// acquire() indexes per-image state (present semaphores, backbuffer handles) by the driver's
			// image index, which ranges over the full image array. A swapchain we cannot track in full is
			// unusable, so this is a hard failure. Never acquired from, so immediate destroy is legal.
			if (count > MAX_SWAPCHAIN_IMAGES)
			{
				EMBER_ERROR("vulkan: swapchain has {} images, ember supports {}", count, MAX_SWAPCHAIN_IMAGES);
				vkDestroySwapchainKHR(backend.context.device, data.swapchain, nullptr);
				data.swapchain = VK_NULL_HANDLE; // consistent "no swapchain"; next acquire retries a full build
				return Build::Failed;
			}

			EMBER_VK_CHECK(vkGetSwapchainImagesKHR(backend.context.device, data.swapchain, &count, images));

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
				EMBER_VK_CHECK(vkCreateImageView(backend.context.device, &view_info, nullptr, &view));
				EMBER_VK_CHECK(
					vkCreateSemaphore(backend.context.device, &semaphore_info, nullptr, &data.present_semaphores[i]));

				data.images[i] = backend.resources.textures.insert(
					vk::TextureHot{.image = images[i], .sampled_view = view},
					vk::TextureCold{
						.extent		= {extent.width, extent.height, 1},
						.format		= data.surface_format.format,
						.owns_image = false, // the swapchain owns these; destroy() must skip them
					});

				vk::set_name(
					backend.context, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(images[i]), "ember.backbuffer");
			}

			data.image_count = count;
			return Build::Ok;
		}
	}

	SwapchainHandle Device::create_swapchain(const SwapchainDef& def) noexcept
	{
		if (m_backend == nullptr)
			return {};

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());
		if (m_backend->context.platform == nullptr)
		{
			EMBER_ERROR("gpu: cannot create a swapchain on a headless device");
			return {};
		}

		auto& swapchains			 = m_backend->resources.swapchains;
		const SwapchainHandle handle = swapchains.emplace();

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: swapchain pool exhausted");
			return {};
		}

		vk::SwapchainData& data	   = *swapchains.get(handle);
		data.window				   = def.window;
		data.requested_mode		   = def.present_mode;
		data.preferred_image_count = def.image_count;

		// Surface first: it outlives every swapchain generation for this window.
		data.surface = platform::vk::create_surface(
			m_backend->context.platform->native_window(def.window), m_backend->context.instance);

		if (data.surface == VK_NULL_HANDLE)
		{
			EMBER_ERROR("vulkan: surface creation failed");
			(void)swapchains.erase(handle);
			return {};
		}

		// Suspended (created while minimized) is fine: the first acquire after restore builds.
		if (build(*m_backend, data) == Build::Failed)
		{
			destroy_swapchain_data(*m_backend, data);
			(void)swapchains.erase(handle);
			return {};
		}

		// Per-slot acquire semaphores, created once: frames_in_flight is fixed at boot, so a
		// recreate never needs more.
		const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

		for (u32 i = 0; i < m_backend->context.frames_in_flight; ++i)
			EMBER_VK_CHECK(
				vkCreateSemaphore(m_backend->context.device, &semaphore_info, nullptr, &data.acquire_semaphores[i]));

		return handle;
	}

	TextureHandle Device::acquire(SwapchainHandle handle) noexcept
	{
		if (m_backend == nullptr)
			return {};

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());
		EMBER_ASSERT(m_backend->frame.open && "acquire outside begin_frame/end_frame");

		if (!m_backend->resources.swapchains.contains(handle))
		{
			EMBER_ASSERT(false && "Invalid SwapchainHandle");
			return {};
		}

		vk::SwapchainData& data = *m_backend->resources.swapchains.get(handle);

		// Same-frame cache: only the first acquire per frame pays for anything.
		const u64 frame_token = m_backend->frame.index + 1;
		if (data.acquired_frame == frame_token)
			return data.images[data.acquired_image];

		// Minimized: report suspended without touching the swapchain; it revives on restore.
		const glm::uvec2 pixels = m_backend->context.platform->window_pixel_size(data.window);
		if (pixels.x == 0 || pixels.y == 0)
			return {};

		const bool extent_changed = pixels.x != data.extent.width || pixels.y != data.extent.height;

		if (data.needs_recreate || extent_changed || data.swapchain == VK_NULL_HANDLE)
		{
			if (build(*m_backend, data) != Build::Ok)
				return {};

			data.needs_recreate = false;
		}

		const u32 slot = static_cast<u32>(m_backend->frame.index % m_backend->context.frames_in_flight);

		// OUT_OF_DATE signals nothing and leaves the semaphore unsignaled, so retrying with the
		// same semaphore is safe. Bounded: one recreate attempt, then give up this frame.
		for (u32 attempt = 0; attempt < 2; ++attempt)
		{
			u32 image_index		  = vk::NO_IMAGE;
			const VkResult result = vkAcquireNextImageKHR(
				m_backend->context.device,
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

				EMBER_ASSERT(m_backend->frame.pending_present_count < MAX_SWAPCHAINS);
				m_backend->frame.pending_presents[m_backend->frame.pending_present_count++] = {
					.swapchain	 = handle,
					.image_index = image_index,
				};

				return data.images[image_index];
			}

			if (result == VK_ERROR_OUT_OF_DATE_KHR)
			{
				if (build(*m_backend, data) != Build::Ok)
					return {};

				continue;
			}

			vk::note_result(*m_backend, result);
			EMBER_ERROR("vulkan: vkAcquireNextImageKHR failed: {}", vk::result_name(result));
			return {};
		}

		return {};
	}

	Extent2D Device::swapchain_extent(SwapchainHandle handle) const noexcept
	{
		if (m_backend == nullptr)
			return {};

		if (auto* data = m_backend->resources.swapchains.get(handle))
			return {data->extent.width, data->extent.height};

		return {};
	}

	void Device::destroy(SwapchainHandle handle) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());

		vk::SwapchainData* data = m_backend->resources.swapchains.get(handle);

		if (data == nullptr)
			return;

		// Remove a pending presentation of this swapchain.
		PendingPresent* pending = m_backend->frame.pending_presents;
		u32& count				= m_backend->frame.pending_present_count;

		for (u32 i = 0; i < count;)
		{
			if (pending[i].swapchain == handle)
				pending[i] = pending[--count]; // unordered remove: batch order carries no meaning
			else
				++i;
		}

		destroy_swapchain_data(*m_backend, *data);
		(void)m_backend->resources.swapchains.erase(handle);
	}
}
