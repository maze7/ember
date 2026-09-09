#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/gpu/texture.h>
#include <ember/memory/memory.h>
#include <gpu/vulkan/common.h>

#include <vk_mem_alloc.h>

namespace ember::gpu
{
	struct Backend;
}

namespace ember::gpu::vk
{
	/**
	 * A staging buffer only this batch reads. Out of frame uploads cannot use the ring, so they
	 * get one of these instead.
	 */
	struct OneOffStaging
	{
		VkBuffer buffer			 = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
	};

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
		u64 value			= 0; // reusable once the upload timeline passes this; 0 = never used.

		/**
		 * Freed when the batch's value passes, not through the destroy queue: the upload clock is
		 * what proves these copies are done, and the frame clock knows nothing about them.
		 */
		Vector<OneOffStaging> one_offs{&memory::heap(MemoryTag::Graphics)};
	};

	/// One kind of batch. A command pool serves exactly one queue family, so critical work on the
	/// graphics family and streamed work on the DMA family cannot share a ring.
	struct UploadRing
	{
		VkCommandPool pool = VK_NULL_HANDLE; // RESET_COMMAND_BUFFER: batches reset individually
											 // because they cross frame-slot boundaries.
		UploadBatch batches[MAX_FRAMES_IN_FLIGHT + 1]{};
		VkCommandBuffer open_cmd = VK_NULL_HANDLE;
		u32 open_index			 = 0;
	};

	/// Ring 0 is critical work and rides the graphics queue; ring 1 is streamed and rides the DMA
	/// queue. Indexed by the streamed flag, so the two never mix.
	inline constexpr u32 UPLOAD_RING_CRITICAL = 0;
	inline constexpr u32 UPLOAD_RING_STREAMED = 1;
	inline constexpr u32 UPLOAD_RING_COUNT	  = 2;

	/// A texture's shape, for the image staging paths.
	struct TextureUpload
	{
		VkImage image		 = VK_NULL_HANDLE;
		TextureFormat format = TextureFormat::Undefined;
		Extent3D extent		 = {1, 1, 1};
		u32 mip_count		 = 1;
		u32 layer_count		 = 1;
		VkImageLayout steady = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
		UploadRing rings[UPLOAD_RING_COUNT]{};

		/**
		 * Uploads keep their own clock. A batch submits on its own and signals the next value;
		 * the frame waits for that value instead of carrying the copies itself. Two things fall
		 * out: a copy reaches the GPU without a frame to ride, and asking whether one landed is
		 * a counter read rather than a wait.
		 */
		VkSemaphore timeline = VK_NULL_HANDLE;
		u64 value			 = 0; // last value handed to an upload submit
		u64 completed		 = 0; // highest value proven signalled; polled, never waited on
		u64 critical_value	 = 0; // last value a frame must wait for; streamed uploads never raise it

		/// The DMA family is its own: streamed images change hands on the way out and the
		/// graphics queue takes them back at promotion. False when the adapter has no dedicated
		/// transfer family, and then none of that machinery runs.
		bool cross_family = false;
	};

	/// The value the batch now open will signal. A streamed resource records this at creation:
	/// it is resident once the upload timeline reaches it.
	[[nodiscard]] inline u64 pending_upload_value(const Staging& staging) noexcept { return staging.value + 1; }

	[[nodiscard]] bool staging_boot(Backend& backend, u64 per_slot_bytes) noexcept;

	/// Raw teardown for destroy_boot_state: GPU already idle, deferred queue already drained.
	void staging_destroy(const Context& ctx, Staging& staging) noexcept;

	/// Resets the slot's ring slice. The caller's timeline wait proved it reclaimable.
	void staging_begin_frame(Staging& staging, u32 slot) noexcept;

	/// Records a staged copy into the current upload batch (opening it if needed).
	/// Source memory comes from the ring while a frame s open, else a one-off buffer that
	/// rides the desttroy queue. Owner thread only.
	void
	staging_upload(Backend& backend, VkBuffer dst, u64 dst_offset, Span<const u8> data, bool streamed = false) noexcept;

	/// Uploads the whole subresource chain (layer-major, mip-minor, tightly packed
	/// blocks) and leaves the image in its steady layout. Empty data records only
	/// the UNDEFINED to steady transition, which is how creation christens every
	/// texture into a known layout. Owner thread only.
	void staging_upload_texture(
		Backend& backend, const TextureUpload& upload, Span<const u8> data, bool streamed = false) noexcept;

	/// One subresource, steady to copy and back. The batch's entry barrier orders
	/// all prior submitted work before the copy, so frames in flight are safe.
	void staging_update_texture(
		Backend& backend, const TextureUpload& upload, u32 mip, u32 layer, Span<const u8> data) noexcept;

	/**
	 * Closes the open batch and submits it on its own, signalling the next upload value, which
	 * it returns. Returns the last value handed out when there was nothing to send, so a caller
	 * can always wait on what comes back.
	 */
	u64 submit_uploads(Backend& backend) noexcept;

	/// Reads how far the upload timeline has got. Never blocks; a frame that finds nothing new
	/// simply carries the previous answer.
	void poll_uploads(Backend& backend) noexcept;

	/**
	 * Records the graphics-side half of a streamed image's ownership transfer: the release the
	 * DMA batch wrote hands the image over, and this takes it back. Layouts and families must
	 * match the release exactly, which is why both halves live in this file.
	 */
	void staging_acquire_image(
		Backend& backend,
		VkCommandBuffer cmd,
		VkImage image,
		VkImageAspectFlags aspect,
		u32 mips,
		u32 layers,
		VkImageLayout layout) noexcept;
}
