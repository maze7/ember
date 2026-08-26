#include <ember/core/common.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/destroy_queue.h>
#include <gpu/vulkan/staging.h>

#include <cstring>

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
		[[nodiscard]] StagingAlloc staging_alloc(Backend& backend, u64 size) noexcept
		{
			StagingRing& ring = backend.staging.ring;

			if (backend.frame.open)
			{
				const u64 aligned = align_up(ring.cursor, STAGING_ALIGN);

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

			// The timeline_value + 1 is the submit that will consume this copy, so we can kill the
			// buffer the moment that timeline_value has provably completed.
			backend.destroy_queue.destroy(buffer, allocation);

			return {
				.buffer		= buffer,
				.allocation = allocation,
				.offset		= 0,
				.cpu		= static_cast<u8*>(result.pMappedData),
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

		[[nodiscard]] VkCommandBuffer upload_cmd(Backend& backend) noexcept
		{
			Staging& staging = backend.staging;

			if (staging.open_cmd != VK_NULL_HANDLE)
				return staging.open_cmd;

			for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT + 1; ++i)
			{
				if (staging.batches[i].value > backend.frame.completed)
					continue;

				VkCommandBufferBeginInfo begin_info{
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, // implicit reset (pool flag)
				};

				EMBER_VK_CHECK(vkBeginCommandBuffer(staging.batches[i].cmd, &begin_info));
				record_upload_barrier(staging.batches[i].cmd, true);

				staging.open_cmd   = staging.batches[i].cmd;
				staging.open_index = i;
				return staging.open_cmd;
			}

			// fif+1 batches with at most fif in flight: unreachable unless the frame model broke.
			EMBER_ASSERT(false && "upload batch pool exhausted");
			return VK_NULL_HANDLE;
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

		// RESET_COMMAND_BUFFER breaks the whole-pool-reset pattern deliberately: batches
		// cross frame-slot boundaries (out-of-frame recording), so slot pools can't own them.
		const VkCommandPoolCreateInfo pool_info{
			.sType			  = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags			  = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = backend.context.graphics.family,
		};

		if (vkCreateCommandPool(backend.context.device, &pool_info, nullptr, &staging.pool) != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: upload command pool creation failed");
			return false;
		}

		VkCommandBuffer commands[MAX_FRAMES_IN_FLIGHT + 1];
		const VkCommandBufferAllocateInfo allocate_info{
			.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool		= staging.pool,
			.level				= VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = MAX_FRAMES_IN_FLIGHT + 1,
		};

		if (vkAllocateCommandBuffers(backend.context.device, &allocate_info, commands) != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: upload command-buffer allocation failed");
			return false;
		}

		for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT + 1; ++i)
			staging.batches[i].cmd = commands[i];

		return true;
	}

	void staging_destroy(const Context& ctx, Staging& staging) noexcept
	{
		if (staging.pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(ctx.device, staging.pool, nullptr); // frees the batches with it

		if (staging.ring.buffer != VK_NULL_HANDLE)
			vmaDestroyBuffer(ctx.allocator, staging.ring.buffer, staging.ring.allocation);

		staging = {};
	}

	void staging_begin_frame(Staging& staging, u32 slot) noexcept
	{
		staging.ring.cursor	   = u64{slot} * staging.ring.slice_bytes;
		staging.ring.slice_end = staging.ring.cursor + staging.ring.slice_bytes;
	}

	void staging_upload(Backend& backend, VkBuffer dst, u64 dst_offset, Span<const u8> data) noexcept
	{
		const StagingAlloc src = staging_alloc(backend, data.size());

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

		VkCommandBuffer cmd = upload_cmd(backend);
		if (cmd == VK_NULL_HANDLE)
			return;

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

	VkCommandBuffer close_upload(Backend& backend, u64 value) noexcept
	{
		Staging& staging = backend.staging;

		if (staging.open_cmd == VK_NULL_HANDLE)
			return VK_NULL_HANDLE;

		record_upload_barrier(staging.open_cmd, false);
		EMBER_VK_CHECK(vkEndCommandBuffer(staging.open_cmd));

		staging.batches[staging.open_index].value = value;

		VkCommandBuffer cmd = staging.open_cmd;
		staging.open_cmd	= VK_NULL_HANDLE;
		return cmd;
	}
}
