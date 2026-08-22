#pragma once

#include "ember/gpu/common.h"
#include <ember/gpu/swapchain.h>
#include <gpu/vulkan/device_state.h>

namespace ember::gpu::vk
{
	[[nodiscard]] BufferHandle create_buffer(DeviceState& backend, const BufferDef& def) noexcept;

	void destroy_buffer(DeviceState& backend, BufferHandle handle) noexcept;
	void update_buffer(DeviceState& backend, BufferHandle handle, u64 offset, Span<const u8> data) noexcept;
}
