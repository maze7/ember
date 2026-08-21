/// The one translation unit that instantiates VulkanMemoryAllocator. Warnings from the
/// header are silenced by the SYSTEM include path; no other TU may define VMA_IMPLEMENTATION.
#include "common.h" // volk first: VK_NO_PROTOTYPES before vulkan.h

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
