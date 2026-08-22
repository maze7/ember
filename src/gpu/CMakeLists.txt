# Ember::Gpu is the RHI abstraction layer. Exactly one backend is compiled in.
if (NOT EMBER_ENABLE_PLATFORM)
	message(FATAL_ERROR "EMBER_ENABLE_GRAPHICS requires EMBER_ENABLE_PLATFORM (swapchains need windows)")
endif()

if (NOT EMBER_USE_VULKAN)
	message(FATAL_ERROR "EMBER_ENABLE_GRAPHICS requires a backend: enable EMBER_USE_VULKAN")
endif()

ember_add_module(Gpu
	SOURCES
		validation.cpp
		vulkan/device.cpp
		vulkan/device_buffer.cpp
		vulkan/backend.cpp
		vulkan/vma.cpp
		vulkan/swapchain.cpp
		vulkan/destroy_queue.cpp
	PUBLIC_DEPS
		Ember::Core
		Ember::Platform
	PRIVATE_DEPS
		volk::volk
		vma::vma
)

# VK_NO_PROTOTYPES globally: any TU that includes vulkan.h before volk.h would otherwise #error.
target_compile_definitions(ember_gpu
	PRIVATE
		EMBER_GPU_VULKAN=1
		VK_NO_PROTOTYPES
)
