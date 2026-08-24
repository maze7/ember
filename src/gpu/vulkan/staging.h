#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <gpu/vulkan/common.h>

#include <vk_mem_alloc.h>

namespace ember::gpu
{
	struct Backend;
}

namespace ember::gpu::vk
{
	/**
	 * An upload batch: one command buffer of vkCmdCopyBuffer2 calls bracketed by fat global
	 * barriers, submitted ahead of the frame's main commands (same vkQueueSubmit2).
	 *
	 * Opened lazily on the first update; when no frame is open (loading screens, boot-time
	 * initial_data) the batch simply stays open and rides the next end_frame or wait_idle.
	 * One code path for both cases, no special init-time upload machinery.
	 *
	 * fif + 1 batches suffice by construction: at most one un-retired batch per in-flight
	 * frame plus the open one. Acquisition therefore never waits and never queries the
	 * timeline, frame pacing already proved what completed (FrameState::completed).
	 */
	struct UploadBatch
	{
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		u64 value			= 0; // reusable once the frame timeline passes this; 0 = never used.
	};

	struct StagingRing
	{
		VkBuffer buffer			 = VK_NULL_HANDLE; // raw on purpose: staging is never shader-visible,
		VmaAllocation allocation = VK_NULL_HANDLE; // so it earns no pool slot and no bindless index.
		u8* cpu					 = nullptr;
		u64 slice_bytes			 = 0;
		u64 cursor				 = 0; // plain u64: update_buffer is owner-thread by contract
		u64 slice_end			 = 0;
		bool coherent			 = true;
	};

	struct Staging
	{
		StagingRing ring{};
		VkCommandPool pool = VK_NULL_HANDLE; // RESET_COMMAND_BUFFER: batches reset individually
											 // because they cross frame-slot boundaries.
		UploadBatch batches[MAX_FRAMES_IN_FLIGHT + 1]{};
		VkCommandBuffer open_cmd = VK_NULL_HANDLE;
		u32 open_index = 0;
	};

	[[nodiscard]] bool staging_boot(Backend& backend, u64 per_slot_bytes) noexcept;

	/// Raw teardown for destroy_boot_state: GPU already idle, deferred queue already drained.
	void staging_destroy(const Context& ctx, Staging& staging) noexcept;

	/// Resets the slot's ring slice. The caller's timeline wait proved it reclaimable.
	void staging_begin_frame(Staging& staging, u32 slot) noexcept;

	/// Records a staged copy into the current upload batch (opening it if needed).
	/// Source memory comes from the ring while a frame s open, else a one-off buffer that
	/// rides the desttroy queue. Owner thread only.
	void staging_upload(Backend& backend, VkBuffer dst, u64 dst_offset, Span<const u8> data) noexcept;

	/// Ends the open batch (exit barrier + vkEndCommandBuffer), stamps it with the value
	/// the consuming submit will signal, and returns its command buffer. Null when nothing to do.
	[[nodiscard]] VkCommandBuffer close_upload(Backend& backend, u64 value) noexcept;
}
