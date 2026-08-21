#pragma once

#if EMBER_USE_VULKAN

	#include <ember/containers/span.h>
	#include <ember/core/common.h>
	#include <ember/platform/window.h>
	#include <volk.h>

/**
 * Vulkan window-system glue, owned by Ember::Platform so the GPU module never touches SDL.
 */
namespace ember::platform::vk
{
	/// Instance extensions the window system needs (VK_KHR_surface + platform surface).
	/// Emtpy when video is not initialized (headless)
	[[nodiscard]] Span<const char* const> instance_extensions() noexcept;

	/// vkGetInstanceProcAddr from the loader the platform opened (recounted; release_loader
	/// balances it). Null when no Vulkan driver/loader is installed
	[[nodiscard]] PFN_vkGetInstanceProcAddr get_instance_proc_addr() noexcept;

	/// Balances get_instance_proc_addr's library reference. Call once at device shutdown.
	void release_loader() noexcept;

	/// True when `queue_family` of `physical_device` can present to this platform's windows.
	bool presentation_supported(VkInstance instance, VkPhysicalDevice physical_device, u32 queue_family) noexcept;

	/// Creates a surface for one of this Platform's windows. Caller owns the surface.
	VkSurfaceKHR create_surface(NativeWindow window, VkInstance instance) noexcept;

	/// Destroys a surface created with `create_surface()`
	void destroy_surface(VkInstance instance, VkSurfaceKHR surface) noexcept;
}

#endif // EMBER_USE_VULKAN
