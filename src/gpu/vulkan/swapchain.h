#pragma once

#include <ember/gpu/swapchain.h>
#include <gpu/vulkan/device_state.h>

namespace ember::gpu::vk
{
	/**
	 * Creates surface + swapchain + semaphores + backbuffer pool entries. False on failure, leaving `data` fully
	 * cleaned up.
	 */
	[[nodiscard]] bool swapchain_create(DeviceState& backend, const SwapchainDef& def, SwapchainData& data) noexcept;

	/**
	 * Destroys everything swapchain_create made. Every native object goes through the destroy queue, so frames
	 * still in flight retire before anything dies; safe to call mid-run.
	 */
	void swapchain_destroy(DeviceState& backend, SwapchainData& data) noexcept;

	/**
	 * Acquires the next backbuffer for `handle` (must be live in backend.resources.swapchains), rebuilding on
	 * resize and OUT_OF_DATE/SUBOPTIMAL. Returns a null handle while minimized or on failure. A successful acquire
	 * queues the swapchain in backend.pending_presents; the same-frame cache keeps that to once per frame.
	 */
	[[nodiscard]] TextureHandle swapchain_acquire(DeviceState& backend, SwapchainHandle handle) noexcept;
}
