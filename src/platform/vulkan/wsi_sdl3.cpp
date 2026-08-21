#include <SDL3/SDL_vulkan.h>
#include <ember/core/logger.h>
#include <platform/vulkan/wsi.h>

/**
 * Vulkan window-system glue, owned by Ember::Platform so the GPU module never touches SDL.
 */
namespace ember::platform::vk
{
	[[nodiscard]] Span<const char* const> instance_extensions() noexcept
	{
		u32 count				 = 0;
		const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);

		if (names == nullptr)
		{
			// Headless (video not initialized) is a legitimate configuration, not an error
			EMBER_TRACE("SDL_Vulkan_GetInstanceExtensions: {}", SDL_GetError());
			return {};
		}

		return {names, static_cast<size_t>(count)};
	}

	[[nodiscard]] PFN_vkGetInstanceProcAddr get_instance_proc_addr() noexcept
	{
		// Refcounted inside SDL; SDL_CreateWindow(SDL_WINDOW_VULKAN) holds its own reference.
		if (!SDL_Vulkan_LoadLibrary(nullptr))
		{
			EMBER_ERROR("SDL_Vulkan_LoadLibrary failed: {}", SDL_GetError());
			return nullptr;
		}

		auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());

		if (!proc)
			EMBER_ERROR("SDL_Vulkan_GetVkGetInstanceProcAddr failed: {}", SDL_GetError());

		return proc;
	}

	void release_loader() noexcept { SDL_Vulkan_UnloadLibrary(); }

	bool presentation_supported(VkInstance instance, VkPhysicalDevice physical_device, u32 queue_family) noexcept
	{
		return SDL_Vulkan_GetPresentationSupport(instance, physical_device, queue_family);
	}

	VkSurfaceKHR create_surface(NativeWindow window, VkInstance instance) noexcept
	{
		if (!window.is_type(WindowBackend::Sdl3) || !window)
		{
			EMBER_ERROR("vulkan_wsi: NativeWindow is not a live SDL3 window");
			return VK_NULL_HANDLE;
		}

		VkSurfaceKHR surface{};

		if (!SDL_Vulkan_CreateSurface((SDL_Window*)window.value, instance, nullptr, &surface))
		{
			EMBER_ERROR("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
			return VK_NULL_HANDLE;
		}

		return surface;
	}

	void destroy_surface(VkInstance instance, VkSurfaceKHR surface) noexcept
	{
		if (surface != VK_NULL_HANDLE)
			SDL_Vulkan_DestroySurface(instance, surface, nullptr);
	}
}
