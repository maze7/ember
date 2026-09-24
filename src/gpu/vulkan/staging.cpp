#include <ember/core/common.h>
#include <ember/core/profile.h>
#include <ember/sync/thread.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/destroy_queue.h>
#include <gpu/vulkan/formats.h>
#include <gpu/vulkan/staging.h>

#include <cstring>
#include <mutex>
#include <numeric>

namespace ember::gpu::vk
{
	namespace
	{
		/// 16 keeps memcpy on its vector path; buffer-buffer copies themselves need no
		/// alignment. Image copies thread the format's block size through region_alignment.
		constexpr u64 STAGING_ALIGN = 16;

		struct StagingAlloc
		{
			VkBuffer buffer			 = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE; // non-null only for one-offs
			u64 offset				 = 0;
			u8* cpu					 = nullptr;
		};

		[[nodiscard]] bool is_owner(const Backend& backend) noexcept
		{
			return backend.owner_thread == current_thread_id();
		}

		/**
		 * Staging for one copy, under the lock. The frame ring serves the owner inside a frame
		 * (begin_frame's wait proved the slice free); everything else is a one-off buffer that the
		 * open batch owns and frees once its value completes. Out-of-frame and off-thread uploads
		 * can never use the ring: no wait has proven any slice free to them, and load-time volume
		 * shouldn't be bounded by ring size anyway. Load-time cost is dominated by IO and decode,
		 * not by VMA allocations.
		 */
		[[nodiscard]] StagingAlloc staging_alloc(Backend& backend, UploadRing& upload, u64 size, u64 alignment,
												 bool frame_ring) noexcept
		{
			StagingRing& ring = backend.staging.ring;

			if (frame_ring && backend.frame.open)
			{
				EMBER_ASSERT(is_owner(backend) && "the frame ring is the owner thread's");

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

			if (auto vr = vmaCreateBuffer(backend.context.allocator, &buffer_info, &alloc_info, &buffer, &allocation,
										  &result);
				vr != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: one-off staging of {} bytes failed: {}", size, result_name(vr));
				return {};
			}

			// The batch that is about to copy out of this is the only reader, and its own value is
			// what proves the copy done. The destroy queue runs on the frame clock and would free
			// this too early.
			EMBER_ASSERT(upload.open_cmd != VK_NULL_HANDLE && "a one-off belongs to an open batch");
			upload.batches[upload.open_index].one_offs.push_back({buffer, allocation});

			return {
				.buffer		= buffer,
				.allocation = allocation,
				.offset		= 0,
				.cpu		= static_cast<u8*>(result.pMappedData),
			};
		}

		/**
		 * The bytes are in: publishes them for the copy and lets the batch submit. Pairs with a
		 * claim made under the lock that counted the caller as a writer. A flush on coherent
		 * memory is a defined no-op, so one-offs just call it unconditionally.
		 */
		void release_write(Backend& backend, UploadRing& upload, const StagingAlloc& src, u64 size) noexcept
		{
			if (src.allocation != VK_NULL_HANDLE)
				(void)vmaFlushAllocation(backend.context.allocator, src.allocation, 0, size);
			else if (!backend.staging.ring.coherent)
				(void)vmaFlushAllocation(backend.context.allocator, backend.staging.ring.allocation, src.offset, size);

			upload.writers.fetch_sub(1, std::memory_order_release);
		}

		[[nodiscard]] constexpr u64 region_alignment(const FormatInfo& info) noexcept
		{
			return std::lcm<u64>(16, info.block_bytes);
		}

		[[nodiscard]] constexpr VkExtent3D mip_extent(Extent3D extent, u32 mip) noexcept
		{
			return {
				extent.width >> mip > 0 ? extent.width >> mip : 1,
				extent.height >> mip > 0 ? extent.height >> mip : 1,
				extent.depth >> mip > 0 ? extent.depth >> mip : 1,
			};
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

		/// The ring's open batch, opened here if there is none. Under the lock.
		[[nodiscard]] VkCommandBuffer upload_cmd(Backend& backend, UploadRing& upload) noexcept
		{
			if (upload.open_cmd != VK_NULL_HANDLE)
				return upload.open_cmd;

			const u64 completed = upload.completed.load(std::memory_order_acquire);

			for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT + 1; ++i)
			{
				// The ring's own clock, not the frame's: a batch is reusable once its own submit
				// signalled, whether or not a frame has been anywhere near it.
				if (upload.batches[i].value > completed)
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

		void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, u32 base_mip, u32 mips,
						   u32 base_layer, u32 layers, VkImageLayout from, VkImageLayout to, bool entry,
						   u32 src_family = VK_QUEUE_FAMILY_IGNORED, u32 dst_family = VK_QUEUE_FAMILY_IGNORED) noexcept
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

		/// Closes the ring's open batch and submits it on its queue, signalling the ring's next
		/// value. Under the lock, with no writer left on the batch.
		void submit_ring(Backend& backend, u32 ring) noexcept
		{
			UploadRing& upload = backend.staging.rings[ring];

			EMBER_ASSERT(upload.open_cmd != VK_NULL_HANDLE);
			EMBER_ASSERT(upload.writers.load(std::memory_order_relaxed) == 0);

			record_upload_barrier(upload.open_cmd, false);
			EMBER_VK_CHECK(vkEndCommandBuffer(upload.open_cmd));

			const u64 value							= ++upload.value;
			upload.batches[upload.open_index].value = value;

			const VkCommandBufferSubmitInfo cmd_info{
				.sType		   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				.commandBuffer = upload.open_cmd,
			};

			const VkSemaphoreSubmitInfo signal{
				.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = upload.timeline,
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
		if (auto vr = vmaCreateBuffer(backend.context.allocator, &buffer_info, &alloc_info, &staging.ring.buffer,
									  &staging.ring.allocation, &result);
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

		set_name(backend.context, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(staging.ring.buffer),
				 "ember.staging_ring");

		// A pool serves one queue family, so each ring gets its own: critical work stays with
		// graphics, streamed work goes to the DMA family. Without a dedicated transfer family the
		// two are the same index and the second pool is simply a duplicate.
		staging.cross_family = backend.context.transfer.family != backend.context.graphics.family;

		const u32 families[UPLOAD_RING_COUNT]	   = {backend.context.graphics.family, backend.context.transfer.family};
		const char* const names[UPLOAD_RING_COUNT] = {"ember.upload_timeline.critical",
													  "ember.upload_timeline.streamed"};

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

			const VkSemaphoreTypeCreateInfo timeline_type{
				.sType		   = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
				.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
				.initialValue  = 0,
			};

			const VkSemaphoreCreateInfo semaphore_info{
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
				.pNext = &timeline_type,
			};

			if (vkCreateSemaphore(backend.context.device, &semaphore_info, nullptr, &upload.timeline) != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: upload timeline creation failed");
				return false;
			}

			set_name(backend.context, VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<u64>(upload.timeline), names[ring]);
		}

		return true;
	}

	void staging_destroy(const Context& ctx, Staging& staging) noexcept
	{
		for (UploadRing& upload : staging.rings)
		{
			if (upload.timeline != VK_NULL_HANDLE)
				vkDestroySemaphore(ctx.device, upload.timeline, nullptr);

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

		// Field by field: the batches own PMR vectors and the rings own atomics, which a
		// whole-struct reset cannot rebind.
		staging.ring = {};

		for (UploadRing& upload : staging.rings)
		{
			upload.pool		  = VK_NULL_HANDLE;
			upload.open_cmd	  = VK_NULL_HANDLE;
			upload.open_index = 0;
			upload.timeline	  = VK_NULL_HANDLE;
			upload.value	  = 0;
			upload.completed.store(0, std::memory_order_relaxed);
			upload.writers.store(0, std::memory_order_relaxed);

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

	void staging_upload(Backend& backend, VkBuffer dst, u64 dst_offset, u64 size, BufferWriter write,
						void* context) noexcept
	{
		EMBER_ASSERT(is_owner(backend) && "buffer uploads are the owner thread's");

		UploadRing& upload = backend.staging.rings[UPLOAD_RING_CRITICAL];
		StagingAlloc src;

		// The copy is recorded before its bytes exist: the GPU only reads them at submit, and the
		// writer count holds the submit back until they are in.
		{
			std::lock_guard lock(backend.staging.lock);

			VkCommandBuffer cmd = upload_cmd(backend, upload);
			if (cmd == VK_NULL_HANDLE)
				return;

			src = staging_alloc(backend, upload, size, STAGING_ALIGN, true);

			if (src.cpu == nullptr)
			{
				EMBER_ERROR("gpu: staging allocation failed; {}-byte update dropped", size);
				return;
			}

			const VkBufferCopy2 region{
				.sType	   = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
				.srcOffset = src.offset,
				.dstOffset = dst_offset,
				.size	   = size,
			};

			const VkCopyBufferInfo2 copy_info{
				.sType		 = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
				.srcBuffer	 = src.buffer,
				.dstBuffer	 = dst,
				.regionCount = 1,
				.pRegions	 = &region,
			};

			vkCmdCopyBuffer2(cmd, &copy_info);
			upload.writers.fetch_add(1, std::memory_order_relaxed);
		}

		// The caller fills the staged bytes in place; the copying form's writer is a memcpy.
		write(src.cpu, size, context);
		release_write(backend, upload, src, size);
	}

	void staging_upload(Backend& backend, VkBuffer dst, u64 dst_offset, Span<const u8> data) noexcept
	{
		Span<const u8> source = data;

		staging_upload(
			backend, dst, dst_offset, data.size(), [](u8* cpu, u64 size, void* context) noexcept
			{ std::memcpy(cpu, static_cast<const Span<const u8>*>(context)->data(), size); }, &source);
	}

	void submit_uploads(Backend& backend, bool all) noexcept
	{
		EMBER_PROFILE_SCOPE_C("gpu: submit uploads", PROFILE_COLOR_IO);

		for (u32 ring = 0; ring < UPLOAD_RING_COUNT; ++ring)
		{
			UploadRing& upload = backend.staging.rings[ring];

			for (u32 spins = 0;; ++spins)
			{
				{
					std::lock_guard lock(backend.staging.lock);

					if (upload.open_cmd == VK_NULL_HANDLE)
						break;

					// No writer can join the batch while the lock is held, so zero here is final.
					if (upload.writers.load(std::memory_order_acquire) == 0)
					{
						submit_ring(backend, ring);
						break;
					}

					// Only the owner fills the critical batch, and it is done by the time it submits.
					EMBER_ASSERT(ring == UPLOAD_RING_STREAMED && "a critical upload is still being written");
				}

				// A streamed batch mid fill goes next time; nothing waits on streamed work. Idle
				// is the exception: it promises that everything requested has been sent.
				if (!all)
					break;

				ember::detail::cpu_relax(spins);
			}
		}
	}

	void poll_uploads(Backend& backend) noexcept
	{
		for (UploadRing& upload : backend.staging.rings)
		{
			// value is the owner's, written under the lock by submits the owner made; reading it
			// here, on the owner, needs no lock.
			if (upload.timeline == VK_NULL_HANDLE || upload.completed.load(std::memory_order_relaxed) >= upload.value)
				continue;

			u64 counter = 0;
			if (vkGetSemaphoreCounterValue(backend.context.device, upload.timeline, &counter) == VK_SUCCESS)
				upload.completed.store(counter, std::memory_order_release);
		}
	}

	u64 staging_upload_texture(Backend& backend, const TextureUpload& upload, Span<const u8> data,
							   bool streamed) noexcept
	{
		UploadRing& ring	   = backend.staging.rings[streamed ? UPLOAD_RING_STREAMED : UPLOAD_RING_CRITICAL];
		const FormatInfo& info = format_info(upload.format);

		// Empty data: just the christening transition into the steady layout.
		// Depth targets take this path, so the color-only rule starts below it.
		if (data.empty())
		{
			std::lock_guard lock(backend.staging.lock);

			VkCommandBuffer cmd = upload_cmd(backend, ring);
			if (cmd == VK_NULL_HANDLE)
				return 0;

			image_barrier(cmd, upload.image, info.aspect, 0, upload.mip_count, 0, upload.layer_count,
						  VK_IMAGE_LAYOUT_UNDEFINED, upload.steady, false);

			return pending_upload_value(ring);
		}

		EMBER_ASSERT(info.aspect == VK_IMAGE_ASPECT_COLOR_BIT && "depth-stencil uploads are out of contract");

		// The whole chain from one buffer of its own, laid out exactly as the copies read it: the
		// caller validated the chain size, and every subresource is whole blocks, so the running
		// offset stays a multiple of the block size the copy rule asks for.
		StagingAlloc src;
		u64 value = 0;

		{
			std::lock_guard lock(backend.staging.lock);

			VkCommandBuffer cmd = upload_cmd(backend, ring);
			if (cmd == VK_NULL_HANDLE)
				return 0;

			src = staging_alloc(backend, ring, data.size(), region_alignment(info), false);

			if (src.cpu == nullptr)
			{
				EMBER_ERROR("gpu: staging failed; texture upload of {} bytes dropped", data.size());
				return 0;
			}

			// One barrier pair brackets the whole chain; the copies land in between.
			image_barrier(cmd, upload.image, info.aspect, 0, upload.mip_count, 0, upload.layer_count,
						  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);

			u64 cursor = 0;
			for (u32 layer = 0; layer < upload.layer_count; ++layer)
			{
				VkBufferImageCopy2 regions[MAX_MIP_LEVELS];

				for (u32 mip = 0; mip < upload.mip_count; ++mip)
				{
					regions[mip] = {
						.sType			  = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
						.bufferOffset	  = src.offset + cursor,
						.imageSubresource = {info.aspect, mip, layer, 1},
						.imageExtent	  = mip_extent(upload.extent, mip),
					};

					cursor += subresource_bytes(info, upload.extent, mip);
				}

				const VkCopyBufferToImageInfo2 copy{
					.sType			= VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
					.srcBuffer		= src.buffer,
					.dstImage		= upload.image,
					.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					.regionCount	= upload.mip_count,
					.pRegions		= regions,
				};

				vkCmdCopyBufferToImage2(cmd, &copy);
			}

			EMBER_ASSERT(cursor == data.size() && "the caller validates the chain size; this is the tripwire");

			// A streamed image leaves the DMA family here and the graphics queue takes it back when
			// the texture is promoted. Both halves name the same layouts and the same families.
			const bool release	 = streamed && backend.staging.cross_family;
			const u32 src_family = release ? backend.context.transfer.family : VK_QUEUE_FAMILY_IGNORED;
			const u32 dst_family = release ? backend.context.graphics.family : VK_QUEUE_FAMILY_IGNORED;

			image_barrier(cmd, upload.image, info.aspect, 0, upload.mip_count, 0, upload.layer_count,
						  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, upload.steady, false, src_family, dst_family);

			ring.writers.fetch_add(1, std::memory_order_relaxed);
			value = pending_upload_value(ring);
		}

		// The long part, outside the lock: the batch cannot submit until the writer count drops.
		std::memcpy(src.cpu, data.data(), data.size());
		release_write(backend, ring, src, data.size());

		return value;
	}

	void staging_update_texture(Backend& backend, const TextureUpload& upload, u32 mip, u32 layer,
								Span<const u8> data) noexcept
	{
		EMBER_ASSERT(is_owner(backend) && "texture updates are the owner thread's");

		const FormatInfo& info = format_info(upload.format);
		EMBER_ASSERT(info.aspect == VK_IMAGE_ASPECT_COLOR_BIT && "depth-stencil uploads are out of contract");

		// An update targets a live resource: the frame always waits for it, so it is critical.
		UploadRing& ring = backend.staging.rings[UPLOAD_RING_CRITICAL];
		StagingAlloc src;

		{
			std::lock_guard lock(backend.staging.lock);

			VkCommandBuffer cmd = upload_cmd(backend, ring);
			if (cmd == VK_NULL_HANDLE)
				return;

			src = staging_alloc(backend, ring, data.size(), region_alignment(info), true);

			if (src.cpu == nullptr)
			{
				EMBER_ERROR("gpu: staging failed; texture mip {} layer {} update dropped", mip, layer);
				return;
			}

			// Round-trip the one subresource so whole-image layout tracking stays true.
			image_barrier(cmd, upload.image, info.aspect, mip, 1, layer, 1, upload.steady,
						  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);

			const VkBufferImageCopy2 region{
				.sType			  = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
				.bufferOffset	  = src.offset,
				.imageSubresource = {info.aspect, mip, layer, 1},
				.imageExtent	  = mip_extent(upload.extent, mip),
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

			image_barrier(cmd, upload.image, info.aspect, mip, 1, layer, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						  upload.steady, false);

			ring.writers.fetch_add(1, std::memory_order_relaxed);
		}

		std::memcpy(src.cpu, data.data(), data.size());
		release_write(backend, ring, src, data.size());
	}

	void staging_acquire_image(Backend& backend, VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
							   u32 mips, u32 layers, VkImageLayout layout) noexcept
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
