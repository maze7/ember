#pragma once

#include <ember/gpu/swapchain.h>
#include <gpu/vulkan/backend.h>

namespace ember::gpu::vk
{
	/**
	 * Creates surface + swapchain + semaphores + backbuffer pool entries. False on failure, leaving `data` fully
	 * cleaned up.
	 */
	[[nodiscard]] bool swapchain_create(DeviceBackend& backend, const SwapchainDef& def, SwapchainData& data) noexcept;

	/**
	 * Destroys everything swapchain_create made. Waits for the device to idle first: teardown and explicit
	 * user destruction are rare, cold paths.
	 */
	void swapchain_destroy(DeviceBackend& backend, SwapchainData& data) noexcept;

	/**
	 * Acquires the next backbuffer for `handle` (must be live in backend.resources.swapchains), handling resize
	 * (lazy recreate), minimize (returns null handle, suspended) and OUT_OF_DATE/SUBOPTIMAL. `slot` selects the
	 * acquire semaphore; the same frame's end_frame submit must wait it. A successful acquire queues the swapchain
	 * in backend.pending_presents; the same-frame cache keeps that to once per frame.
	 */
	[[nodiscard]] TextureHandle swapchain_acquire(DeviceBackend& backend, SwapchainHandle handle, u32 slot) noexcept;
}
