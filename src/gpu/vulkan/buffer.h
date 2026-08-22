#pragma once

#include "ember/gpu/common.h"
#include <ember/gpu/swapchain.h>
#include <gpu/vulkan/backend.h>

namespace ember::gpu::vk
{
	[[nodiscard]] BufferHandle create_buffer(DeviceBackend& backend, const BufferDef& def) noexcept;

	void destroy_buffer(DeviceBackend& backend, BufferHandle handle) noexcept;
	void update_buffer(DeviceBackend& backend, BufferHandle handle, u64 offset, Span<const u8> data) noexcept;
}
