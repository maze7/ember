#include <ember/core/common.h>
#include <ember/core/profile.h>
#include <ember/gpu/device.h>
#include <ember/memory/memory.h>
#include <ember/sync/thread.h>
#include <gpu/vulkan/backend.h>

#include <cmath>
#include <utility>

namespace ember::gpu
{
	namespace
	{
		/// One device at a time: pool indices are bindless slots, so two devices sharing
		/// handle types would alias each other's heaps. Mirrors the Platform claim guard.
		constinit std::atomic<bool> s_device_claimed{false};

		/**
		 * Placeholder visual until CommandList lands: clear every acquired backbuffer with a
		 * slowly cycling hue. Animated on purpose — a static clear can't prove frame pacing.
		 * Pure recording: reads which images to clear, writes only the command buffer.
		 */
		void record_placeholder_clears(
			const FrameState& frame, const vk::ResourcePools& resources, VkCommandBuffer cmd) noexcept
		{
			const f32 t = static_cast<f32>(frame.index) * 0.02f;
			const VkClearColorValue clear{
				.float32 = {
					0.5f + 0.5f * std::sin(t),
					0.5f + 0.5f * std::sin(t + 2.09f),
					0.5f + 0.5f * std::sin(t + 4.19f),
					1.0f,
				}};

			for (u32 i = 0; i < frame.pending_present_count; ++i)
			{
				const vk::SwapchainData& data = resources.swapchains.get(frame.pending_presents[i].swapchain);
				const vk::TextureHot& hot	  = resources.textures.get(data.images[data.acquired_image]);

				// Acquired contents are undefined; the acquire-semaphore wait is scoped to
				// COLOR_ATTACHMENT_OUTPUT, which is why both barriers pivot on that stage.
				const VkImageMemoryBarrier2 to_color{
					.sType			  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
					.srcStageMask	  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					.srcAccessMask	  = VK_ACCESS_2_NONE,
					.dstStageMask	  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					.dstAccessMask	  = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
					.oldLayout		  = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout		  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					.image			  = hot.image,
					.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};

				VkDependencyInfo dependency{
					.sType					 = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					.imageMemoryBarrierCount = 1,
					.pImageMemoryBarriers	 = &to_color,
				};
				vkCmdPipelineBarrier2(cmd, &dependency);

				const VkRenderingAttachmentInfo color{
					.sType		 = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
					.imageView	 = hot.sampled_view,
					.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					.loadOp		 = VK_ATTACHMENT_LOAD_OP_CLEAR,
					.storeOp	 = VK_ATTACHMENT_STORE_OP_STORE,
					.clearValue	 = {.color = clear},
				};

				const VkRenderingInfo rendering{
					.sType				  = VK_STRUCTURE_TYPE_RENDERING_INFO,
					.renderArea			  = {{0, 0}, data.extent},
					.layerCount			  = 1,
					.colorAttachmentCount = 1,
					.pColorAttachments	  = &color,
				};

				// A clear-only pass: dynamic rendering with zero draws is complete and valid.
				vkCmdBeginRendering(cmd, &rendering);
				vkCmdEndRendering(cmd);

				const VkImageMemoryBarrier2 to_present{
					.sType			  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
					.srcStageMask	  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					.srcAccessMask	  = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
					.dstStageMask	  = VK_PIPELINE_STAGE_2_NONE, // the present semaphore takes over
					.dstAccessMask	  = VK_ACCESS_2_NONE,
					.oldLayout		  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					.newLayout		  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
					.image			  = hot.image,
					.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};

				dependency.pImageMemoryBarriers = &to_present;
				vkCmdPipelineBarrier2(cmd, &dependency);
			}
		}

		/**
		 * One submit: wait every acquire semaphore, run the frame's commands, signal the
		 * timeline (CPU pacing) and every acquired image's present semaphore (WSI).
		 */
		void submit_frame(Backend& backend, VkCommandBuffer cmd, u32 slot, u64 value) noexcept
		{
			const FrameState& frame = backend.frame;

			VkSemaphoreSubmitInfo waits[MAX_SWAPCHAINS];
			VkSemaphoreSubmitInfo signals[MAX_SWAPCHAINS + 1];
			u32 wait_count	 = 0;
			u32 signal_count = 0;

			for (u32 i = 0; i < frame.pending_present_count; ++i)
			{
				const vk::SwapchainData& data = backend.resources.swapchains.get(frame.pending_presents[i].swapchain);

				waits[wait_count++] = {
					.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = data.acquire_semaphores[slot],
					.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				};

				signals[signal_count++] = {
					.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = data.present_semaphores[data.acquired_image],
					.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				};
			}

			signals[signal_count++] = {
				.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = frame.timeline,
				.value	   = value,
				.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			};

			const VkCommandBufferSubmitInfo cmd_info{
				.sType		   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				.commandBuffer = cmd,
			};

			const VkSubmitInfo2 submit_info{
				.sType					  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
				.waitSemaphoreInfoCount	  = wait_count,
				.pWaitSemaphoreInfos	  = waits,
				.commandBufferInfoCount	  = 1,
				.pCommandBufferInfos	  = &cmd_info,
				.signalSemaphoreInfoCount = signal_count,
				.pSignalSemaphoreInfos	  = signals,
			};

			vk::note_result(backend, vkQueueSubmit2(backend.context.graphics.handle, 1, &submit_info, VK_NULL_HANDLE));
		}

		/// Batched present: every swapchain touched this frame in one call, results per entry.
		void present_pending(Backend& backend) noexcept
		{
			const FrameState& frame = backend.frame;

			if (frame.pending_present_count == 0)
				return;

			VkSwapchainKHR swapchains[MAX_SWAPCHAINS];
			VkSemaphore present_waits[MAX_SWAPCHAINS];
			u32 image_indices[MAX_SWAPCHAINS];
			VkResult results[MAX_SWAPCHAINS];

			for (u32 i = 0; i < frame.pending_present_count; ++i)
			{
				const vk::SwapchainData& data = backend.resources.swapchains.get(frame.pending_presents[i].swapchain);

				swapchains[i]	 = data.swapchain;
				present_waits[i] = data.present_semaphores[data.acquired_image];
				image_indices[i] = frame.pending_presents[i].image_index;
			}

			const VkPresentInfoKHR present_info{
				.sType				= VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
				.waitSemaphoreCount = frame.pending_present_count,
				.pWaitSemaphores	= present_waits,
				.swapchainCount		= frame.pending_present_count,
				.pSwapchains		= swapchains,
				.pImageIndices		= image_indices,
				.pResults			= results,
			};

			vk::note_result(backend, vkQueuePresentKHR(backend.context.graphics.handle, &present_info));

			// Per-swapchain outcome: OUT_OF_DATE/SUBOPTIMAL here means recreate at next acquire.
			for (u32 i = 0; i < frame.pending_present_count; ++i)
				if (results[i] == VK_ERROR_OUT_OF_DATE_KHR || results[i] == VK_SUBOPTIMAL_KHR)
					backend.resources.swapchains.get(frame.pending_presents[i].swapchain).needs_recreate = true;
		}
	}

	Device::Device(const DeviceDef& def) noexcept
	{
		EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);
		EMBER_ASSERT(::ember::gpu::is_valid(def));

		if (s_device_claimed.exchange(true, std::memory_order_acq_rel))
		{
			EMBER_ERROR("gpu: only one Device may exist at a time");
			return;
		}

		// Publish before boot: shutdown() is the single rollback path for every failure
		// inside boot, and it keys off m_state. A failed boot nulls m_state again, so
		// the Device reads falsy and the claim is released.
		m_state = memory::new_object<Backend>(MemoryTag::Graphics);

		if (!vk::boot(*m_state, def))
			shutdown();
	}

	Device::~Device() noexcept { shutdown(); }

	void Device::shutdown() noexcept
	{
		EMBER_GPU_GUARD();

		// Retire in dependency order: GPU idle -> surviving user resources -> deferred
		// natives -> boot state. Every step tolerates the partial-boot case.
		if (m_state->context.device != VK_NULL_HANDLE)
		{
			(void)vkDeviceWaitIdle(m_state->context.device);

			destroy_resources();
			vk::drain_deferred_destroys(m_state->context, m_state->deferred, UINT64_MAX);
		}

		Backend* dead = std::exchange(m_state, nullptr);
		vk::destroy_boot_state(*dead);
		memory::delete_object(MemoryTag::Graphics, dead);

		s_device_claimed.store(false, std::memory_order_release);
	}

	void Device::destroy_resources() noexcept
	{
		EMBER_GPU_GUARD();

		// Destroy swapchains
		auto& swapchains = m_state->resources.swapchains;
		for (auto it = swapchains.begin(); it != swapchains.end();)
		{
			const SwapchainHandle handle = it.handle();
			++it;
			EMBER_WARN("gpu: swapchain leaked at device destruction");
			destroy(handle);
		}

		// Destroy buffers
		auto& buffers = m_state->resources.buffers;
		for (auto it = buffers.begin(); it != buffers.end();)
		{
			const BufferHandle handle = it.handle();
			++it;
			EMBER_WARN("gpu: buffer leaked at device destruction");
			destroy(handle);
		}
	}

	void Device::wait_idle() noexcept
	{
		EMBER_GPU_GUARD();
		EMBER_PROFILE_SCOPE_C("gpu: wait_idle", PROFILE_COLOR_WAIT);

		vk::note_result(*m_state, vkDeviceWaitIdle(m_state->context.device));

		// Idle means everything signaled: even entries stamped for a submit that never
		// happened (an open frame at teardown) are safe now.
		vk::drain_deferred_destroys(m_state->context, m_state->deferred, UINT64_MAX);
	}

	const DeviceCaps& Device::caps() const noexcept
	{
		// A falsy Device still answers caps(): all-zero caps read as "nothing supported", the
		// least surprising thing guard omitted user code can observe.
		static constinit DeviceCaps s_null_caps{};
		return m_state != nullptr ? m_state->context.caps : s_null_caps;
	}

	bool Device::device_lost() const noexcept
	{
		// acquire pairs with note_result's exchange: a true here happens-after the loss.
		return m_state != nullptr && m_state->lost.load(std::memory_order_acquire);
	}

	u32 Device::validation_error_count() noexcept
	{
		// Tests read these after teardown has joined.
		return debug_state().errors.load(std::memory_order_relaxed);
	}

	u32 Device::validation_warning_count() noexcept { return debug_state().warnings.load(std::memory_order_relaxed); }

	FrameInfo Device::begin_frame() noexcept
	{
		EMBER_GPU_GUARD({});

		const Context& ctx = m_state->context;
		FrameState& frame  = m_state->frame;

		EMBER_ASSERT(!frame.open && "begin_frame called twice without end_frame");

		const u32 slot		 = static_cast<u32>(frame.index % ctx.frames_in_flight);
		const u64 wait_value = frame.slots[slot].submitted;

		/// The one wait that makes everything safe to reuse: this slot's previous submit has
		/// fully retired, so its command pools, deletion bucket and ring slice are free.
		/// Host-side wait: parks the thread, no queue round-trip.
		if (wait_value != 0)
		{
			EMBER_PROFILE_SCOPE_C("gpu: wait_frame", PROFILE_COLOR_WAIT);

			VkSemaphoreWaitInfo wait_info{
				.sType			= VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
				.semaphoreCount = 1,
				.pSemaphores	= &frame.timeline,
				.pValues		= &wait_value,
			};

			vk::note_result(*m_state, vkWaitSemaphores(ctx.device, &wait_info, UINT64_MAX));
		}

		EMBER_VK_CHECK(vkResetCommandPool(ctx.device, frame.slots[slot].pool, 0));

		// The wait above proved wait_value completed; the graveyard rides the frame pacing
		// and needs no extra queries.
		vk::drain_deferred_destroys(ctx, m_state->deferred, wait_value);
		frame.pending_present_count = 0;
		frame.open					= true;

		return {.frame_index = static_cast<u32>(frame.index), .slot = slot};
	}

	void Device::end_frame() noexcept
	{
		EMBER_GPU_GUARD();

		const Context& ctx = m_state->context;
		FrameState& frame  = m_state->frame;

		EMBER_ASSERT(frame.open && "end_frame without begin_frame");

		const u32 slot	= static_cast<u32>(frame.index % ctx.frames_in_flight);
		const u64 value = ++frame.timeline_value;

		VkCommandBuffer cmd = frame.slots[slot].commands;

		const VkCommandBufferBeginInfo begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		EMBER_VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

		record_placeholder_clears(frame, m_state->resources, cmd);

		EMBER_VK_CHECK(vkEndCommandBuffer(cmd));

		submit_frame(*m_state, cmd, slot, value);
		present_pending(*m_state);

		frame.slots[slot].submitted = value;
		frame.open					= false;
		++frame.index;
	}
}
