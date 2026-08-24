#include <gpu/vulkan/common.h>
#include <gpu/vulkan/device_state.h>

namespace ember::gpu::vk
{
	void set_name(const Context& ctx, VkObjectType type, u64 handle, const char* name) noexcept
	{
		if (ctx.device == VK_NULL_HANDLE || name == nullptr)
			return;

		VkDebugUtilsObjectNameInfoEXT info{
			.sType		  = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.pNext		  = nullptr,
			.objectType	  = type,
			.objectHandle = handle,
			.pObjectName  = name,
		};

		(void)vkSetDebugUtilsObjectNameEXT(ctx.device, &info);
	}

	void note_result(Backend& backend, VkResult result) noexcept
	{
		if (result == VK_ERROR_DEVICE_LOST) [[unlikely]]
		{
			if (!backend.lost.exchange(true, std::memory_order_acq_rel))
				EMBER_ERROR("vulkan: device lost (TDR or driver fault); the Device must be recreated");
		}
	}
}
