# Ember::Gpu is the RHI abstraction layer. Exactly one backend is compiled in.
if (NOT EMBER_ENABLE_PLATFORM)
	message(FATAL_ERROR "EMBER_ENABLE_GRAPHICS requires EMBER_ENABLE_PLATFORM (swapchains need windows)")
endif()

if (NOT EMBER_USE_VULKAN)
	message(FATAL_ERROR "EMBER_ENABLE_GRAPHICS requires a backend: enable EMBER_USE_VULKAN")
endif()

# The RHI's vocabulary: handles, enums and Defs, which need no backend to use. Code that describes
# GPU state without driving a device (material types, and through them the cook tool) links this
# alone, so an edit to the backend relinks no tool and recooks no shader.
add_library(ember_gpu_types INTERFACE)
add_library(Ember::GpuTypes ALIAS ember_gpu_types)
target_link_libraries(ember_gpu_types INTERFACE Ember::Core)

ember_add_module(Gpu
	SOURCES
		vulkan/common.cpp
		vulkan/device.cpp
		vulkan/device_boot.cpp
		vulkan/device_buffer.cpp
		vulkan/device_swapchain.cpp
		vulkan/device_pipeline.cpp
		vulkan/device_texture.cpp
		vulkan/device_sampler.cpp
		vulkan/vma.cpp
		vulkan/destroy_queue.cpp
		vulkan/staging.cpp
		vulkan/transient_ring.cpp
		vulkan/descriptor_heap.cpp
		vulkan/command_list.cpp
	PUBLIC_DEPS
		Ember::Core
		Ember::GpuTypes
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

# Designated initializers are used for a lot of vulkan structs. This keeps the warnings from polluting
target_compile_options(ember_gpu PRIVATE
	$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wno-missing-field-initializers>
)
