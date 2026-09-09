#include <ember/core/common.h>
#include <ember/core/profile.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/destroy_queue.h>
#include <gpu/vulkan/formats.h>
#include <gpu/vulkan/staging.h>

#include <cstring>
#include <numeric>

namespace ember::gpu::vk
{
	namespace
	{
		/// 16 keeps memcpy on its vector path; buffer-buffer copies themselves need no
		/// alignment. Stage 2 threads caps.copy_offset_alignment through here for images.
		constexpr u64 STAGING_ALIGN = 16;

		struct StagingAlloc
		{
			VkBuffer buffer			 = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE; // non-null only for one-offs
			u64 offset				 = 0;
			u8* cpu					 = nullptr;
		};

		/**
		 * Ring while a frame is open (reclaim is proven by begin_frame's wait); one-off
		 * buffer otherwise. Out-of-frame allocations can never use the ring: no wait has
		 * proven any slice free, and load-time volume shouldn't be bounded by ring size
		 * anyway. Load-time cost is dominated by IO and decode, not by VMA allocations.
		 */
		[[nodiscard]] StagingAlloc staging_alloc(Backend& backend, u32 ring_index, u64 size, u64 alignment) noexcept
		{
			StagingRing& ring = backend.staging.ring;

			if (backend.frame.open)
			{
				const u64 aligned = align_up(ring.cursor, alignment);

				if (aligned + size <= ring.slice_end)
				{
					ring.cursor = aligned + size;
					return {.buffer = ring.buffer, .offset = aligned, .cpu = ring.cpu + aligned};
				}
			}

			VkBufferCreateInfo buffer_info{
				.sType		 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size		 = size,
				.usage		 = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};

			VmaAllocationCreateInfo alloc_info{};
			alloc_info.flags =
				VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST; // read once by DMA: VRAM would be waste

			VkBuffer buffer			 = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VmaAllocationInfo result{};

			if (auto vr = vmaCreateBuffer(
					backend.context.allocator, &buffer_info, &alloc_info, &buffer, &allocation, &result);
				vr != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: one-off staging of {} bytes failed: {}", size, result_name(vr));
				return {};
			}

			// The batch that is about to copy out of this is the only reader, and its own value is
			// what proves the copy done. The destroy queue runs on the frame clock and would free
			// this too early.
			UploadRing& upload = backend.staging.rings[ring_index];
			EMBER_ASSERT(upload.open_cmd != VK_NULL_HANDLE && "a one-off belongs to an open batch");
			upload.batches[upload.open_index].one_offs.push_back({buffer, allocation});

			return {
				.buffer		= buffer,
				.allocation = allocation,
				.offset		= 0,
				.cpu		= static_cast<u8*>(result.pMappedData),
			};
		}

		[[nodiscard]] constexpr u64 region_alignment(const FormatInfo& info) noexcept
		{
			return std::lcm<u64>(16, info.block_bytes);
		}

		/**
		 * Fat global barriers bracket the batch. Per-resource precision buys nothing at a
		 * once-per-frame phase boundary (the GPU serializes on the worst case regardless)
		 * and costs a tracking system. Entry orders prior-frame access before our writes;
		 * exit makes the writes visible to everything after.
		 */
		void record_upload_barrier(VkCommandBuffer cmd, bool entry) noexcept
		{
			VkMemoryBarrier2 barrier{
				.sType		   = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				.srcStageMask  = entry ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COPY_BIT,
				.srcAccessMask = entry ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT,
				.dstStageMask  = entry ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.dstAccessMask =
					entry ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			};

			VkDependencyInfo dependency{
				.sType				= VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.memoryBarrierCount = 1,
				.pMemoryBarriers	= &barrier,
			};

			vkCmdPipelineBarrier2(cmd, &dependency);
		}

		void free_one_offs(Backend& backend, UploadBatch& batch) noexcept
		{
			for (const OneOffStaging& one_off : batch.one_offs)
				vmaDestroyBuffer(backend.context.allocator, one_off.buffer, one_off.allocation);

			batch.one_offs.clear();
		}

		[[nodiscard]] VkCommandBuffer upload_cmd(Backend& backend, bool streamed) noexcept
		{
			Staging& staging   = backend.staging;
			UploadRing& upload = staging.rings[streamed ? UPLOAD_RING_STREAMED : UPLOAD_RING_CRITICAL];

			if (upload.open_cmd != VK_NULL_HANDLE)
				return upload.open_cmd;

			for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT + 1; ++i)
			{
				// The upload clock, not the frame's: a batch is reusable once its own submit
				// signalled, whether or not a frame has been anywhere near it.
				if (upload.batches[i].value > staging.completed)
					continue;

				free_one_offs(backend, upload.batches[i]);

				VkCommandBufferBeginInfo begin_info{
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, // implicit reset (pool flag)
				};

				EMBER_VK_CHECK(vkBeginCommandBuffer(upload.batches[i].cmd, &begin_info));
				record_upload_barrier(upload.batches[i].cmd, true);

				upload.open_cmd	  = upload.batches[i].cmd;
				upload.open_index = i;
				return upload.open_cmd;
			}

			// fif+1 batches with at most fif in flight: unreachable unless the frame model broke.
			EMBER_ASSERT(false && "upload batch pool exhausted");
			return VK_NULL_HANDLE;
		}

		void image_barrier(
			VkCommandBuffer cmd,
			VkImage image,
			VkImageAspectFlags aspect,
			u32 base_mip,
			u32 mips,
			u32 base_layer,
			u32 layers,
			VkImageLayout from,
			VkImageLayout to,
			bool entry,
			u32 src_family = VK_QUEUE_FAMILY_IGNORED,
			u32 dst_family = VK_QUEUE_FAMILY_IGNORED) noexcept
		{
			// A release names both families and leaves the destination scope empty: the acquire
			// on the other queue supplies it, and the two halves must otherwise match exactly.
			const bool release = src_family != dst_family;

			// The exit side is broad because upload boundaries run per texture, not
			// per frame; precision here would buy nothing.
			const VkImageMemoryBarrier2 barrier{
				.sType				 = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask		 = entry ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COPY_BIT,
				.srcAccessMask		 = entry ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT,
				.dstStageMask		 = release ? VK_PIPELINE_STAGE_2_NONE
									   : entry ? VK_PIPELINE_STAGE_2_COPY_BIT
											   : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				.dstAccessMask		 = release ? VK_ACCESS_2_NONE
									   : entry ? VK_ACCESS_2_TRANSFER_WRITE_BIT
											   : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
				.oldLayout			 = from,
				.newLayout			 = to,
				.srcQueueFamilyIndex = src_family,
				.dstQueueFamilyIndex = dst_family,
				.image				 = image,
				.subresourceRange	 = {aspect, base_mip, mips, base_layer, layers},
			};

			const VkDependencyInfo dependency{
				.sType					 = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers	 = &barrier,
			};

			vkCmdPipelineBarrier2(cmd, &dependency);
		}

		/// Stages one subresource and records its copy. One region per call keeps
		/// the ring and one-off paths uniform: each region names its own buffer,
		/// so ring exhaustion degrades per mip instead of per texture.
		[[nodiscard]] bool copy_subresource(
			Backend& backend,
			VkCommandBuffer cmd,
			const TextureUpload& upload,
			const FormatInfo& info,
			u32 mip,
			u32 layer,
			Span<const u8> bytes,
			u32 ring) noexcept
		{
			const StagingAlloc src = staging_alloc(backend, ring, bytes.size(), region_alignment(info));

			if (src.cpu == nullptr)
			{
				EMBER_ERROR("gpu: staging failed; texture mip {} layer {} dropped", mip, layer);
				return false;
			}

			std::memcpy(src.cpu, bytes.data(), bytes.size());

			if (src.allocation != VK_NULL_HANDLE)
				(void)vmaFlushAllocation(backend.context.allocator, src.allocation, 0, bytes.size());
			else if (!backend.staging.ring.coherent)
				(void)vmaFlushAllocation(
					backend.context.allocator, backend.staging.ring.allocation, src.offset, bytes.size());

			const VkBufferImageCopy2 region{
				.sType			  = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
				.bufferOffset	  = src.offset,
				.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1},
				.imageExtent =
					{
						upload.extent.width >> mip > 0 ? upload.extent.width >> mip : 1,
						upload.extent.height >> mip > 0 ? upload.extent.height >> mip : 1,
						upload.extent.depth >> mip > 0 ? upload.extent.depth >> mip : 1,
					},
			};

			const VkCopyBufferToImageInfo2 copy{
				.sType			= VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
				.srcBuffer		= src.buffer,
				.dstImage		= upload.image,
				.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.regionCount	= 1,
				.pRegions		= &region,
			};

			vkCmdCopyBufferToImage2(cmd, &copy);
			return true;
		}

		void submit_ring(Backend& backend, u32 ring) noexcept
		{
			Staging& staging   = backend.staging;
			UploadRing& upload = staging.rings[ring];

			if (upload.open_cmd == VK_NULL_HANDLE)
				return;

			record_upload_barrier(upload.open_cmd, false);
			EMBER_VK_CHECK(vkEndCommandBuffer(upload.open_cmd));

			const u64 value							= ++staging.value;
			upload.batches[upload.open_index].value = value;

			// Only critical work moves the watermark a frame waits on.
			if (ring == UPLOAD_RING_CRITICAL)
				staging.critical_value = value;

			const VkCommandBufferSubmitInfo cmd_info{
				.sType		   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				.commandBuffer = upload.open_cmd,
			};

			const VkSemaphoreSubmitInfo signal{
				.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = staging.timeline,
				.value	   = value,
				.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			};

			const VkSubmitInfo2 submit_info{
				.sType					  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
				.commandBufferInfoCount	  = 1,
				.pCommandBufferInfos	  = &cmd_info,
				.signalSemaphoreInfoCount = 1,
				.pSignalSemaphoreInfos	  = &signal,
			};

			// Each ring submits to the family its pool was created for; nothing else is legal.
			const VkQueue queue =
				ring == UPLOAD_RING_STREAMED ? backend.context.transfer.handle : backend.context.graphics.handle;

			note_result(backend, vkQueueSubmit2(queue, 1, &submit_info, VK_NULL_HANDLE));

			upload.open_cmd = VK_NULL_HANDLE;
		}
	}

	bool staging_boot(Backend& backend, u64 per_slot_bytes) noexcept
	{
		Staging& staging = backend.staging;

		VkBufferCreateInfo buffer_info{
			.sType		 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size		 = per_slot_bytes * backend.context.frames_in_flight,
			.usage		 = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};

		VmaAllocationCreateInfo alloc_info{};
		alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;

		VmaAllocationInfo result{};
		if (auto vr = vmaCreateBuffer(
				backend.context.allocator,
				&buffer_info,
				&alloc_info,
				&staging.ring.buffer,
				&staging.ring.allocation,
				&result);
			vr != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: staging ring ({} bytes) failed: {}", buffer_info.size, result_name(vr));
			return false;
		}

		VkMemoryPropertyFlags properties = 0;
		vmaGetAllocationMemoryProperties(backend.context.allocator, staging.ring.allocation, &properties);

		staging.ring.cpu		 = static_cast<u8*>(result.pMappedData);
		staging.ring.slice_bytes = per_slot_bytes;
		staging.ring.coherent	 = (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

		set_name(
			backend.context, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(staging.ring.buffer), "ember.staging_ring");

		// A pool serves one queue family, so each ring gets its own: critical work stays with
		// graphics, streamed work goes to the DMA family. Without a dedicated transfer family the
		// two are the same index and the second pool is simply a duplicate.
		staging.cross_family = backend.context.transfer.family != backend.context.graphics.family;

		const u32 families[UPLOAD_RING_COUNT] = {backend.context.graphics.family, backend.context.transfer.family};

		for (u32 ring = 0; ring < UPLOAD_RING_COUNT; ++ring)
		{
			UploadRing& upload = staging.rings[ring];

			const VkCommandPoolCreateInfo pool_info{
				.sType			  = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags			  = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
				.queueFamilyIndex = families[ring],
			};

			if (vkCreateCommandPool(backend.context.device, &pool_info, nullptr, &upload.pool) != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: upload command pool creation failed");
				return false;
			}

			VkCommandBuffer commands[MAX_FRAMES_IN_FLIGHT + 1];
			const VkCommandBufferAllocateInfo allocate_info{
				.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool		= upload.pool,
				.level				= VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = MAX_FRAMES_IN_FLIGHT + 1,
			};

			if (vkAllocateCommandBuffers(backend.context.device, &allocate_info, commands) != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: upload command-buffer allocation failed");
				return false;
			}

			for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT + 1; ++i)
				upload.batches[i].cmd = commands[i];
		}

		const VkSemaphoreTypeCreateInfo timeline_type{
			.sType		   = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
			.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
			.initialValue  = 0,
		};

		const VkSemaphoreCreateInfo semaphore_info{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			.pNext = &timeline_type,
		};

		if (vkCreateSemaphore(backend.context.device, &semaphore_info, nullptr, &staging.timeline) != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: upload timeline creation failed");
			return false;
		}

		set_name(
			backend.context,
			VK_OBJECT_TYPE_SEMAPHORE,
			reinterpret_cast<u64>(staging.timeline),
			"ember.upload_timeline");

		return true;
	}

	void staging_destroy(const Context& ctx, Staging& staging) noexcept
	{
		if (staging.timeline != VK_NULL_HANDLE)
			vkDestroySemaphore(ctx.device, staging.timeline, nullptr);

		for (UploadRing& upload : staging.rings)
		{
			// Idle by contract, so anything still parked here has been read.
			for (UploadBatch& batch : upload.batches)
			{
				for (const OneOffStaging& one_off : batch.one_offs)
					vmaDestroyBuffer(ctx.allocator, one_off.buffer, one_off.allocation);

				batch.one_offs.clear();
			}

			if (upload.pool != VK_NULL_HANDLE)
				vkDestroyCommandPool(ctx.device, upload.pool, nullptr); // frees the batches with it
		}

		if (staging.ring.buffer != VK_NULL_HANDLE)
			vmaDestroyBuffer(ctx.allocator, staging.ring.buffer, staging.ring.allocation);

		// Field by field: the batches own PMR vectors, which a whole-struct reset cannot rebind.
		staging.ring	 = {};
		staging.timeline = VK_NULL_HANDLE;
		staging.value = staging.completed = staging.critical_value = 0;

		for (UploadRing& upload : staging.rings)
		{
			upload.pool		  = VK_NULL_HANDLE;
			upload.open_cmd	  = VK_NULL_HANDLE;
			upload.open_index = 0;

			for (UploadBatch& batch : upload.batches)
			{
				batch.cmd	= VK_NULL_HANDLE;
				batch.value = 0;
			}
		}
	}

	void staging_begin_frame(Staging& staging, u32 slot) noexcept
	{
		staging.ring.cursor	   = u64{slot} * staging.ring.slice_bytes;
		staging.ring.slice_end = staging.ring.cursor + staging.ring.slice_bytes;
	}

	void staging_upload(Backend& backend, VkBuffer dst, u64 dst_offset, Span<const u8> data, bool streamed) noexcept
	{
		const u32 ring = streamed ? UPLOAD_RING_STREAMED : UPLOAD_RING_CRITICAL;

		VkCommandBuffer cmd = upload_cmd(backend, streamed);
		if (cmd == VK_NULL_HANDLE)
			return;

		const StagingAlloc src = staging_alloc(backend, ring, data.size(), STAGING_ALIGN);

		if (src.cpu == nullptr)
		{
			EMBER_ERROR("gpu: staging allocation failed; {}-byte update dropped", data.size());
			return;
		}

		std::memcpy(src.cpu, data.data(), data.size());

		// Publication needs the flush on non-coherent memory; a flush on coherent memory is
		// a defined no-op, so one-offs just call it unconditionally.
		if (src.allocation != VK_NULL_HANDLE)
			(void)vmaFlushAllocation(backend.context.allocator, src.allocation, 0, data.size());
		else if (!backend.staging.ring.coherent)
			(void)vmaFlushAllocation(
				backend.context.allocator, backend.staging.ring.allocation, src.offset, data.size());

		VkBufferCopy2 region{
			.sType	   = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
			.srcOffset = src.offset,
			.dstOffset = dst_offset,
			.size	   = data.size(),
		};

		VkCopyBufferInfo2 copy_info{
			.sType		 = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
			.srcBuffer	 = src.buffer,
			.dstBuffer	 = dst,
			.regionCount = 1,
			.pRegions	 = &region,
		};

		vkCmdCopyBuffer2(cmd, &copy_info);
	}

	u64 submit_uploads(Backend& backend) noexcept
	{
		EMBER_PROFILE_SCOPE_C("gpu: submit uploads", PROFILE_COLOR_IO);

		submit_ring(backend, UPLOAD_RING_CRITICAL);
		submit_ring(backend, UPLOAD_RING_STREAMED);

		return backend.staging.value;
	}

	void poll_uploads(Backend& backend) noexcept
	{
		Staging& staging = backend.staging;

		if (staging.timeline == VK_NULL_HANDLE || staging.completed >= staging.value)
			return;

		u64 counter = 0;
		if (vkGetSemaphoreCounterValue(backend.context.device, staging.timeline, &counter) == VK_SUCCESS)
			staging.completed = counter;
	}

	void
	staging_upload_texture(Backend& backend, const TextureUpload& upload, Span<const u8> data, bool streamed) noexcept
	{
		const u32 ring		   = streamed ? UPLOAD_RING_STREAMED : UPLOAD_RING_CRITICAL;
		const FormatInfo& info = format_info(upload.format);

		VkCommandBuffer cmd = upload_cmd(backend, streamed);
		if (cmd == VK_NULL_HANDLE)
			return;

		// Empty data: just the christening transition into the steady layout.
		// Depth targets take this path, so the color-only rule starts below it.
		if (data.empty())
		{
			image_barrier(
				cmd,
				upload.image,
				info.aspect,
				0,
				upload.mip_count,
				0,
				upload.layer_count,
				VK_IMAGE_LAYOUT_UNDEFINED,
				upload.steady,
				false);
			return;
		}

		EMBER_ASSERT(info.aspect == VK_IMAGE_ASPECT_COLOR_BIT && "depth-stencil uploads are out of contract");

		// One barrier pair brackets the whole chain; the copies land in between.
		image_barrier(
			cmd,
			upload.image,
			info.aspect,
			0,
			upload.mip_count,
			0,
			upload.layer_count,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			true);

		u64 cursor = 0;
		for (u32 layer = 0; layer < upload.layer_count; ++layer)
		{
			for (u32 mip = 0; mip < upload.mip_count; ++mip)
			{
				const u64 bytes = subresource_bytes(info, upload.extent, mip);
				(void)copy_subresource(backend, cmd, upload, info, mip, layer, {data.data() + cursor, bytes}, ring);
				cursor += bytes;
			}
		}

		EMBER_ASSERT(cursor == data.size() && "the caller validates the chain size; this is the tripwire");

		// A streamed image leaves the DMA family here and the graphics queue takes it back when
		// the texture is promoted. Both halves name the same layouts and the same families.
		const bool release	 = streamed && backend.staging.cross_family;
		const u32 src_family = release ? backend.context.transfer.family : VK_QUEUE_FAMILY_IGNORED;
		const u32 dst_family = release ? backend.context.graphics.family : VK_QUEUE_FAMILY_IGNORED;

		image_barrier(
			cmd,
			upload.image,
			info.aspect,
			0,
			upload.mip_count,
			0,
			upload.layer_count,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			upload.steady,
			false,
			src_family,
			dst_family);
	}

	void staging_update_texture(
		Backend& backend, const TextureUpload& upload, u32 mip, u32 layer, Span<const u8> data) noexcept
	{
		const FormatInfo& info = format_info(upload.format);
		EMBER_ASSERT(info.aspect == VK_IMAGE_ASPECT_COLOR_BIT && "depth-stencil uploads are out of contract");

		constexpr bool streamed = false; // an update targets a live resource: the frame always waits for it
		VkCommandBuffer cmd		= upload_cmd(backend, streamed);
		if (cmd == VK_NULL_HANDLE)
			return;

		// Round-trip the one subresource so whole-image layout tracking stays true.
		image_barrier(
			cmd,
			upload.image,
			info.aspect,
			mip,
			1,
			layer,
			1,
			upload.steady,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			true);
		(void)copy_subresource(backend, cmd, upload, info, mip, layer, data, UPLOAD_RING_CRITICAL);
		image_barrier(
			cmd,
			upload.image,
			info.aspect,
			mip,
			1,
			layer,
			1,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			upload.steady,
			false);
	}

	void staging_acquire_image(
		Backend& backend,
		VkCommandBuffer cmd,
		VkImage image,
		VkImageAspectFlags aspect,
		u32 mips,
		u32 layers,
		VkImageLayout layout) noexcept
	{
		// The acquire supplies the destination scope the release left empty; the source scope is
		// empty in turn, because the release already made the writes available.
		const VkImageMemoryBarrier2 barrier{
			.sType				 = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask		 = VK_PIPELINE_STAGE_2_NONE,
			.srcAccessMask		 = VK_ACCESS_2_NONE,
			.dstStageMask		 = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.dstAccessMask		 = VK_ACCESS_2_MEMORY_READ_BIT,
			.oldLayout			 = layout,
			.newLayout			 = layout,
			.srcQueueFamilyIndex = backend.context.transfer.family,
			.dstQueueFamilyIndex = backend.context.graphics.family,
			.image				 = image,
			.subresourceRange	 = {aspect, 0, mips, 0, layers},
		};

		const VkDependencyInfo dependency{
			.sType					 = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers	 = &barrier,
		};

		vkCmdPipelineBarrier2(cmd, &dependency);
	}
}
