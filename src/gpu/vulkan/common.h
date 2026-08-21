#pragma once

/// Volk must be the first Vulkan include in every backend translation unit: it defines
/// VK_NO_PROTOTYPES and pulls in <vulkan/vulkan.h>.
#include <volk.h>

#include <ember/core/common.h>
#include <ember/core/logger.h>

#include <cstdint>
#include <type_traits>

namespace ember::gpu::vk
{
	/// The API version we create the instance/device against. Everything the layer
	/// relies on (dynamic rendering, sync2, timeline semaphors, descriptor indexing)
	/// is core in Vulkan 1.3.
	inline constexpr u32 API_VERSION = VK_API_VERSION_1_3;

	/// Human-readable VkResult for logs. Not exhaustive; unknown codes print numerically.
	[[nodiscard]] inline const char* result_name(VkResult result) noexcept
	{
		switch (result)
		{
			case VK_SUCCESS:
				return "VK_SUCCESS";
			case VK_NOT_READY:
				return "VK_NOT_READY";
			case VK_TIMEOUT:
				return "VK_TIMEOUT";
			case VK_EVENT_SET:
				return "VK_EVENT_SET";
			case VK_EVENT_RESET:
				return "VK_EVENT_RESET";
			case VK_INCOMPLETE:
				return "VK_INCOMPLETE";
			case VK_SUBOPTIMAL_KHR:
				return "VK_SUBOPTIMAL_KHR";
			case VK_ERROR_OUT_OF_HOST_MEMORY:
				return "VK_ERROR_OUT_OF_HOST_MEMORY";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY:
				return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
			case VK_ERROR_INITIALIZATION_FAILED:
				return "VK_ERROR_INITIALIZATION_FAILED";
			case VK_ERROR_DEVICE_LOST:
				return "VK_ERROR_DEVICE_LOST";
			case VK_ERROR_MEMORY_MAP_FAILED:
				return "VK_ERROR_MEMORY_MAP_FAILED";
			case VK_ERROR_LAYER_NOT_PRESENT:
				return "VK_ERROR_LAYER_NOT_PRESENT";
			case VK_ERROR_EXTENSION_NOT_PRESENT:
				return "VK_ERROR_EXTENSION_NOT_PRESENT";
			case VK_ERROR_FEATURE_NOT_PRESENT:
				return "VK_ERROR_FEATURE_NOT_PRESENT";
			case VK_ERROR_INCOMPATIBLE_DRIVER:
				return "VK_ERROR_INCOMPATIBLE_DRIVER";
			case VK_ERROR_TOO_MANY_OBJECTS:
				return "VK_ERROR_TOO_MANY_OBJECTS";
			case VK_ERROR_FORMAT_NOT_SUPPORTED:
				return "VK_ERROR_FORMAT_NOT_SUPPORTED";
			case VK_ERROR_FRAGMENTED_POOL:
				return "VK_ERROR_FRAGMENTED_POOL";
			case VK_ERROR_UNKNOWN:
				return "VK_ERROR_UNKNOWN";
			case VK_ERROR_OUT_OF_POOL_MEMORY:
				return "VK_ERROR_OUT_OF_POOL_MEMORY";
			case VK_ERROR_INVALID_EXTERNAL_HANDLE:
				return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
			case VK_ERROR_FRAGMENTATION:
				return "VK_ERROR_FRAGMENTATION";
			case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
				return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
			case VK_ERROR_SURFACE_LOST_KHR:
				return "VK_ERROR_SURFACE_LOST_KHR";
			case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
				return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
			case VK_ERROR_OUT_OF_DATE_KHR:
				return "VK_ERROR_OUT_OF_DATE_KHR";
			case VK_ERROR_VALIDATION_FAILED_EXT:
				return "VK_ERROR_VALIDATION_FAILED_EXT";
			case VK_PIPELINE_COMPILE_REQUIRED:
				return "VK_PIPELINE_COMPILE_REQUIRED";
			default:
				return "VK_RESULT_?";
		}
	}
}

/**
 * For calls whose failure is a bug, not a runtime condition: logs the expression, result name and
 * code, then asserts. Runtime-failable calls (instance/device/swapchain creation, present, submit)
 * test their VkResult explicitly instead.
 */
#define EMBER_VK_CHECK(expr)                                                                                           \
	do                                                                                                                 \
	{                                                                                                                  \
		const VkResult ember_vk_result_ = (expr);                                                                      \
		if (ember_vk_result_ != VK_SUCCESS) [[unlikely]]                                                               \
		{                                                                                                              \
			EMBER_ERROR(                                                                                               \
				"vulkan: {} failed: {} ({})",                                                                          \
				#expr,                                                                                                 \
				ember::gpu::vk::result_name(ember_vk_result_),                                                         \
				static_cast<int>(ember_vk_result_));                                                                   \
			EMBER_ASSERT(false && "Vulkan call failed");                                                               \
		}                                                                                                              \
	} while (0)
