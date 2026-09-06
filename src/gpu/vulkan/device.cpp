#include <ember/core/common.h>
#include <ember/core/profile.h>
#include <ember/gpu/device.h>
#include <ember/memory/memory.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/formats.h>

#include <atomic>
#include <utility>

namespace ember::gpu
{
	namespace
	{
		/// One device at a time: pool indicies are bindless slots, so two devices sharing
		/// handle types would alias each other's heaps. Mirrors the Platform claim guard.
		constinit std::atomic<bool> s_device_claimed{false};

		/// Ensure that a frame where no commands were submitted still performs a clear and
		/// prepares the swapchain for presentation. This ensures that all platforms (Wayland)
		/// receive a presentable surface every frame.
		void record_placeholder_clears(Backend& backend, VkCommandBuffer cmd) noexcept
		{
			const VkCommandBufferBeginInfo begin_info{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			EMBER_VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

			const VkClearColorValue clear{
				.float32 = {
					0.02f,
					0.025f,
					0.035f,
					1.0f,
				}};

			for (u32 i = 0; i < backend.frame.pending_present_count; ++i)
			{
				const vk::SwapchainData& data =
					*backend.resources.swapchains.get(backend.frame.pending_presents[i].swapchain);
				const vk::TextureHot& hot = *backend.resources.textures.get(data.images[data.acquired_image]);

				// Acquired contents are undefined; the acquire-semaphore wait in submit_frame is
				// scoped to COLOR_ATTACHMENT_OUTPUT, which is why both barriers pivot on that stage.
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

			EMBER_VK_CHECK(vkEndCommandBuffer(cmd));
		}

		/**
		 * One submit: wait every acquire semaphore, run the upload batch (when present) ahead
		 * of the frame's commands, signal the timeline (CPU pacing) and every acquired image's
		 * present semaphore (WSI). Submission order plus the batch's exit barrier is what makes
		 * this frame's reads see this frame's uploads.
		 */
		void submit_frame(Backend& backend, VkCommandBuffer upload, VkCommandBuffer frame_cmd, u64 value) noexcept
		{
			FrameState& frame = backend.frame;
			const u32 slot	  = static_cast<u32>(frame.index % backend.context.frames_in_flight);

			VkSemaphoreSubmitInfo waits[MAX_SWAPCHAINS];
			VkSemaphoreSubmitInfo signals[MAX_SWAPCHAINS + 1];
			u32 wait_count	 = 0;
			u32 signal_count = 0;

			for (u32 i = 0; i < frame.pending_present_count; ++i)
			{
				const vk::SwapchainData& data = *backend.resources.swapchains.get(frame.pending_presents[i].swapchain);

				waits[wait_count++] = {
					.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = data.acquire_semaphores[slot],
					.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				};

				signals[signal_count++] = {
					.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = data.present_semaphores[data.acquired_image],
					// ALL_COMMANDS: the present transition retires in no stage (dst NONE), so
					// any narrower signal scope fails to cover it.
					.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				};
			}

			signals[signal_count++] = {
				.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = frame.timeline,
				.value	   = value,
				.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			};

			VkCommandBufferSubmitInfo cmd_infos[2];
			u32 cmd_count = 0;

			if (upload != VK_NULL_HANDLE)
				cmd_infos[cmd_count++] = {
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = upload};

			cmd_infos[cmd_count++] = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = frame_cmd};

			const VkSubmitInfo2 submit_info{
				.sType					  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
				.waitSemaphoreInfoCount	  = wait_count,
				.pWaitSemaphoreInfos	  = waits,
				.commandBufferInfoCount	  = cmd_count,
				.pCommandBufferInfos	  = cmd_infos,
				.signalSemaphoreInfoCount = signal_count,
				.pSignalSemaphoreInfos	  = signals,
			};

			vk::note_result(backend, vkQueueSubmit2(backend.context.graphics.handle, 1, &submit_info, VK_NULL_HANDLE));
		}

		/// Batched present: every swapchain touched this frame in one call, results per entry.
		void present_pending(Backend& backend) noexcept
		{
			FrameState& frame = backend.frame;

			if (frame.pending_present_count == 0)
				return;

			VkSwapchainKHR swapchains[MAX_SWAPCHAINS];
			VkSemaphore present_waits[MAX_SWAPCHAINS];
			u32 image_indices[MAX_SWAPCHAINS];
			VkResult results[MAX_SWAPCHAINS];

			for (u32 i = 0; i < frame.pending_present_count; ++i)
			{
				const vk::SwapchainData& data = *backend.resources.swapchains.get(frame.pending_presents[i].swapchain);

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
					backend.resources.swapchains.get(frame.pending_presents[i].swapchain)->needs_recreate = true;
		}

		/**
		 * Submits any open upload batch on its own, signalling the timeline. Out-of-frame
		 * updates (loading screens, boot-time initial_data) reach the GPU through here; inside
		 * a frame the batch rides submit_frame instead.
		 */
		void flush_uploads(Backend& backend) noexcept
		{
			if (backend.staging.open_cmd == VK_NULL_HANDLE)
				return;

			const u64 value			  = ++backend.frame.timeline_value;
			const VkCommandBuffer cmd = vk::close_upload(backend, value);

			const VkCommandBufferSubmitInfo cmd_info{
				.sType		   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				.commandBuffer = cmd,
			};

			const VkSemaphoreSubmitInfo signal{
				.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = backend.frame.timeline,
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

			vk::note_result(backend, vkQueueSubmit2(backend.context.graphics.handle, 1, &submit_info, VK_NULL_HANDLE));
		}
	}

	Device::Device(const DeviceDef& def) noexcept : Device(nullptr, def) {}
	Device::Device(Platform& platform, const DeviceDef& def) noexcept : Device(&platform, def) {}

	Device::Device(Platform* platform, const DeviceDef& def) noexcept
	{
		EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);
		EMBER_ASSERT(::ember::gpu::is_valid(def));

		if (s_device_claimed.exchange(true, std::memory_order_acq_rel))
		{
			EMBER_ERROR("gpu: only one Device may exist at a time");
			return;
		}

		// Published before boot so a failed boot rolls back through the same shutdown path
		// (and releases the claim). Boot logs the adapter summary on success.
		m_backend = memory::new_object<Backend>(MemoryTag::Graphics);

		if (!vk::boot(*this, *m_backend, platform, def))
			shutdown();
	}

	Device::~Device() noexcept { shutdown(); }

	void Device::shutdown() noexcept
	{
		EMBER_GPU_GUARD();

		// Partial boots may not have created a device; without one nothing was ever
		// submitted or created, so only the boot-state teardown below has work.
		if (m_backend->context.device != VK_NULL_HANDLE)
		{
			(void)vkDeviceWaitIdle(m_backend->context.device);

			// Backend-owned pool entries (transient ring, overflow pages) leave first so
			// the sweep below reports only genuine user leaks. Idle makes it legal.
			vk::transient_destroy(*m_backend);
			vk::heap_destroy_fallbacks(*m_backend);

			// Destroy surviving user resources while m_state is still published.
			destroy_resources();

			// Idle means everything signaled: even entries stamped for a submit that never
			// happened (an open frame at teardown) are safe now.
			m_backend->destroy_queue.drain(
				m_backend->context, m_backend->descriptor_heap, m_backend->resources, UINT64_MAX);
		}

		Backend* dead = std::exchange(m_backend, nullptr);
		vk::destroy_boot_state(*dead);
		memory::delete_object(MemoryTag::Graphics, dead);
		s_device_claimed.store(false, std::memory_order_release);
	}

	void Device::destroy_resources() noexcept
	{
		EMBER_GPU_GUARD();

		// Swapchains first: their backbuffers are texture pool entries, and the
		// texture path refuses those by design.
		auto& swapchains = m_backend->resources.swapchains;
		for (auto it = swapchains.begin(); it != swapchains.end();)
		{
			const SwapchainHandle handle = it.handle();
			++it;
			destroy(handle);
		}

		auto& pipelines = m_backend->resources.graphics_pipelines;
		for (auto it = pipelines.begin(); it != pipelines.end();)
		{
			const GraphicsPipelineHandle handle = it.handle();
			++it;
			destroy(handle);
		}

		auto& compute = m_backend->resources.compute_pipelines;
		for (auto it = compute.begin(); it != compute.end();)
		{
			const ComputePipelineHandle handle = it.handle();
			++it;
			destroy(handle);
		}

		auto& textures = m_backend->resources.textures;
		for (auto it = textures.begin(); it != textures.end();)
		{
			const TextureHandle handle = it.handle();
			++it;

			// Internal subresource entries die with their owning texture.
			if (!textures.get_cold(handle)->parent.is_null())
				continue;

			destroy(handle);
		}

		auto& samplers = m_backend->resources.samplers;
		for (auto it = samplers.begin(); it != samplers.end();)
		{
			const SamplerHandle handle = it.handle();
			++it;
			destroy(handle);
		}

		auto& buffers = m_backend->resources.buffers;
		for (auto it = buffers.begin(); it != buffers.end();)
		{
			const BufferHandle handle = it.handle();
			++it;
			destroy(handle);
		}
	}

	void Device::wait_idle() noexcept
	{
		EMBER_GPU_GUARD();
		EMBER_PROFILE_SCOPE_C("gpu: wait_idle", PROFILE_COLOR_WAIT);

		// An open out-of-frame upload batch rides this wait: submit it so "idle" means
		// "every requested copy has landed", which is what loading-screen callers want.
		flush_uploads(*m_backend);

		vk::note_result(*m_backend, vkDeviceWaitIdle(m_backend->context.device));

		// Idle proves every handed-out timeline value signalled, so batch and page
		// recycling may reclaim everything the frame pacing hadn't caught up to yet.
		m_backend->frame.completed = m_backend->frame.timeline_value;
		m_backend->destroy_queue.drain(
			m_backend->context, m_backend->descriptor_heap, m_backend->resources, UINT64_MAX);
	}

	const DeviceCaps& Device::caps() const noexcept
	{
		// A falsy Device still answers caps(): all-zero caps read as "nothing supported", the
		// least surprising thing guard omitted user code can observe.
		static constinit DeviceCaps s_null_caps{};
		return m_backend != nullptr ? m_backend->context.caps : s_null_caps;
	}

	bool Device::device_lost() const noexcept
	{
		// acquire pairs with note_result's exchange: a true here happens-after the loss.
		return m_backend != nullptr && m_backend->lost.load(std::memory_order_acquire);
	}

	TransientAllocator& Device::transient() noexcept
	{
		// Any-thread by contract, so no owner guard. A falsy Device hands back a poisoned
		// allocator: every allocation reports invalid, the same mechanism as between frames.
		static constinit TransientAllocator s_null_allocator{};
		return m_backend != nullptr ? m_backend->transient : s_null_allocator;
	}

	u32 Device::validation_error_count() noexcept
	{
		// Tests read these after teardown has joined.
		return debug_state().errors.load(std::memory_order_relaxed);
	}

	u32 Device::validation_warning_count() noexcept { return debug_state().warnings.load(std::memory_order_relaxed); }

	Span<const GpuZoneTiming> Device::gpu_zones() const noexcept
	{
		if (m_backend == nullptr)
			return {};

		return {m_backend->gpu_zones, m_backend->gpu_zone_count};
	}

	FrameInfo Device::begin_frame() noexcept
	{
		EMBER_GPU_GUARD({});
		EMBER_ASSERT(!m_backend->frame.open && "begin_frame called twice without end_frame");

		FrameState& frame	 = m_backend->frame;
		const u32 slot		 = static_cast<u32>(frame.index % m_backend->context.frames_in_flight);
		const u64 wait_value = frame.slots[slot].submitted;

		/// The one wait that makes everything safe to reuse: this slot's previous submit has
		/// fully retired, so its command pool, ring slices and deferred deletions are free.
		/// Host-side wait: parks the thread, no queue round-trip.
		if (wait_value != 0)
		{
			EMBER_PROFILE_SCOPE_C("gpu: wait_frame", PROFILE_COLOR_WAIT);

			const VkSemaphoreWaitInfo wait_info{
				.sType			= VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
				.semaphoreCount = 1,
				.pSemaphores	= &frame.timeline,
				.pValues		= &wait_value,
			};

			vk::note_result(*m_backend, vkWaitSemaphores(m_backend->context.device, &wait_info, UINT64_MAX));
		}

		// The wait proved wait_value (wait_idle may have proven more; keep the max). Batch
		// acquisition, page recycling and the drain below all key off this, never the semaphore.
		if (wait_value > frame.completed)
			frame.completed = wait_value;

		EMBER_VK_CHECK(vkResetCommandPool(m_backend->context.device, frame.slots[slot].pool, 0));

		// The graveyard rides the frame pacing and needs no extra queries.
		m_backend->destroy_queue.drain(
			m_backend->context, m_backend->descriptor_heap, m_backend->resources, frame.completed);

		// Resolve the slot's zones from the frame that just retired, then hand the
		// query range back. Consuming zone_count keeps a later placeholder frame
		// from re-reading a reset range.
		Recording& recording	  = frame.slots[slot].recording;
		m_backend->gpu_zone_count = 0;

		if (frame.timestamps != VK_NULL_HANDLE && recording.zone_count > 0)
		{
			const u32 base = slot * MAX_GPU_ZONES * 2;
			u64 ticks[MAX_GPU_ZONES * 2];

			const VkResult result = vkGetQueryPoolResults(
				m_backend->context.device,
				frame.timestamps,
				base,
				recording.zone_count * 2,
				sizeof(u64) * recording.zone_count * 2,
				ticks,
				sizeof(u64),
				VK_QUERY_RESULT_64_BIT);

			if (result == VK_SUCCESS)
			{
				const f32 period = m_backend->context.caps.timestamp_period_ns;

				for (u32 i = 0; i < recording.zone_count; ++i)
				{
					const Recording::Zone& zone = recording.zones[i];

					m_backend->gpu_zones[i] = {
						.name		 = zone.name,
						.color		 = zone.color,
						.depth		 = zone.depth,
						.duration_ms = static_cast<f32>(ticks[i * 2 + 1] - ticks[i * 2]) * period / 1'000'000.0f,
					};
				}

				m_backend->gpu_zone_count = recording.zone_count;
			}

			vkResetQueryPool(m_backend->context.device, frame.timestamps, base, MAX_GPU_ZONES * 2);
			recording.zone_count = 0;
		}

		// Rebind the slot's slices: the wait above is what proved them reclaimable.
		vk::staging_begin_frame(m_backend->staging, slot);
		vk::transient_begin_frame(*m_backend, slot);

		frame.pending_present_count				= 0;
		frame.slots[slot].recording.inside_pass = false;
		frame.lists_submitted					= 0;
		frame.open								= true;

		return {.frame_index = static_cast<u32>(frame.index), .slot = slot};
	}

	void Device::end_frame() noexcept
	{
		EMBER_GPU_GUARD();
		EMBER_ASSERT(m_backend->frame.open && "end_frame without begin_frame");

		FrameState& frame = m_backend->frame;
		const u32 slot	  = static_cast<u32>(frame.index % m_backend->context.frames_in_flight);
		const u64 value	  = ++frame.timeline_value;

		VkCommandBuffer cmd = frame.slots[slot].recording.commands;
		EMBER_ASSERT(!frame.list_open && "a command list was begun but never submitted");

		// Frames that submitted nothing still owe every acquired backbuffer a legal
		// present; the boot era clear survives as that fallback.
		if (frame.lists_submitted == 0)
			record_placeholder_clears(*m_backend, cmd);

		// Seal the frame's CPU-written memory before the submit that publishes it: transient
		// flushes and poisons, the upload batch closes stamped with this frame's value.
		vk::transient_end_frame(*m_backend, value);
		const VkCommandBuffer upload = vk::close_upload(*m_backend, value);

		submit_frame(*m_backend, upload, cmd, value);
		present_pending(*m_backend);

		frame.slots[slot].submitted = value;
		frame.open					= false;
		++frame.index;
	}

	CommandList Device::begin_command_list() noexcept
	{
		EMBER_GPU_GUARD({});

		FrameState& frame = m_backend->frame;
		EMBER_ASSERT(frame.open && "command lists record between begin_frame and end_frame");
		EMBER_ASSERT(!frame.list_open && "one command list per frame for now");

		const u32 slot			  = static_cast<u32>(frame.index % m_backend->context.frames_in_flight);
		Recording& recording	  = frame.slots[slot].recording;
		const VkCommandBuffer cmd = recording.commands;

		const VkCommandBufferBeginInfo begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		EMBER_VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

		const VkDescriptorSet sets[] = {
			m_backend->descriptor_heap.heap_set(),
			m_backend->descriptor_heap.constants_set(),
		};
		const u32 zero_offsets[CONSTANT_BUFFER_SLOTS] = {};

		// Descriptor bindings are per bind point; graphics and compute each get the
		// heap and zeroed constant slots so any pipeline is valid before the first
		// set_constants.
		for (const VkPipelineBindPoint bind_point : {VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE})
		{
			vkCmdBindDescriptorSets(
				cmd,
				bind_point,
				m_backend->descriptor_heap.pipeline_layout(),
				0,
				static_cast<u32>(std::size(sets)),
				sets,
				CONSTANT_BUFFER_SLOTS,
				zero_offsets);
		}

		recording		= Recording{.commands = recording.commands};
		frame.list_open = true;

		CommandList list;
		list.m_backend	 = m_backend;
		list.m_recording = &recording;
		return list;
	}

	void Device::submit(CommandList& list) noexcept
	{
		EMBER_GPU_GUARD();

		FrameState& frame = m_backend->frame;
		EMBER_ASSERT(list.m_backend == m_backend && "submit of a foreign command list");
		EMBER_ASSERT(frame.list_open && "submit without begin_command_list");
		EMBER_ASSERT(!list.m_recording->inside_pass && "a render pass is still open at submit");
		EMBER_ASSERT(list.m_recording->zone_depth == 0 && "a GPU zone was begun but never ended");

		EMBER_VK_CHECK(vkEndCommandBuffer(list.m_recording->commands));

		frame.list_open = false;
		++frame.lists_submitted;

		// A sealed list ignores every later call instead of corrupting the next frame.
		list.m_backend	 = nullptr;
		list.m_recording = nullptr;
	}

	TextureFormat Device::swapchain_format(SwapchainHandle handle) const noexcept
	{
		if (m_backend == nullptr)
			return TextureFormat::Undefined;

		const vk::SwapchainData* data = m_backend->resources.swapchains.get(handle);
		if (data == nullptr)
			return TextureFormat::Undefined;

		// Reverse walk of the format table; the surface picker only chooses formats it knows.
		for (u32 i = 0; i < static_cast<u32>(TextureFormat::Count); ++i)
			if (vk::FORMAT_TABLE[i].vk == data->surface_format.format)
				return static_cast<TextureFormat>(i);

		return TextureFormat::Undefined;
	}
}
