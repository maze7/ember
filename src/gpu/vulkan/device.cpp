#include "ember/gpu/common.h"
#include "ember/sync/thread.h"
#include "gpu/validation.h"
#include "gpu/vulkan/swapchain.h"
#include <ember/core/profile.h>
#include <ember/gpu/device.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/buffer.h>
#include <gpu/vulkan/resources.h>
#include <vulkan/vulkan_core.h>

namespace ember::gpu
{
	struct DeviceBackend final : vk::DeviceBackend
	{
	};

	namespace
	{
		/// One device at a time: pool indicies are bindless slots, so two devices sharing
		/// handle types would alias each other's heaps. Mirrors the Platform claim guard.
		constinit std::atomic<bool> s_device_claimed{false};
	}

	Device::Device(const DeviceDef& def) noexcept
	{
		EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);

		if (s_device_claimed.exchange(true, std::memory_order_acq_rel))
		{
			EMBER_ERROR("gpu: only one Device may exist at a time");
			return;
		}

		DeviceBackend* backend = memory::new_object<DeviceBackend>(MemoryTag::Graphics);

		if (!boot(*backend, def))
		{
			memory::delete_object(MemoryTag::Graphics, backend);
			s_device_claimed.store(false, std::memory_order_release);
			return;
		}

		m_backend = backend;
	}

	Device::~Device() noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);
		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());

		// Required ordering:
		// 1. Stop GPU work
		// 2. Destroy all native pooled resources
		// 3. Destroy device/context
		// 4. Release SDL vulkan loader through WSI.
		wait_idle();

		DeviceBackend* backend = std::exchange(m_backend, nullptr);

		// Surviving swapchains: destroy with a warning — user code should have destroyed them.
		for (auto it = backend->resources.swapchains.begin(); it != backend->resources.swapchains.end(); ++it)
		{
			EMBER_WARN("gpu: swapchain leaked at device destruction");
			vk::swapchain_destroy(*backend, *it);
		}
		backend->resources.swapchains.clear();

		shutdown(*backend);
		memory::delete_object(MemoryTag::Graphics, backend);

		s_device_claimed.store(false, std::memory_order_release);
	}

	void Device::wait_idle() noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());
		EMBER_PROFILE_SCOPE_C("gpu: wait_idle", PROFILE_COLOR_WAIT);

		vk::note_result(*m_backend, vkDeviceWaitIdle(m_backend->device));

		// Idle means everything signaled: even entries stamped for a submit that never
		// happened (an open frame at teardown) are safe now.
		vk::drain_deferred_destroys(*m_backend, UINT64_MAX);
	}

	const DeviceCaps& Device::caps() const noexcept
	{
		// A falsy Device still answers caps(): all-zero caps read as "nothing supported", the
		// least surprising thing guard omitted user code can observe.
		static constinit DeviceCaps s_null_caps{};
		return m_backend != nullptr ? m_backend->caps : s_null_caps;
	}

	bool Device::device_lost() const noexcept
	{
		// acquire pairs with note_result's exchange: a true here happens-after the loss.
		return m_backend != nullptr && m_backend->lost.load(std::memory_order_acquire);
	}

	u32 Device::validation_error_count() noexcept
	{
		// Tests read these after teardown has joined.
		return vk::debug_state().errors.load(std::memory_order_relaxed);
	}

	u32 Device::validation_warning_count() noexcept
	{
		return vk::debug_state().warnings.load(std::memory_order_relaxed);
	}

	FrameInfo Device::begin_frame() noexcept
	{
		if (m_backend == nullptr)
			return {};

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());
		EMBER_ASSERT(!m_backend->frame_open && "begin_frame called twice without end_frame");

		u32 slot	   = static_cast<u32>(m_backend->frame_index % m_backend->frames_in_flight);
		u64 wait_value = m_backend->slots[slot].submitted;

		/// The one wait that makes everything safe to reuse: this slot's previous submit has
		/// fully retired, so its command pools, deletion bucket and ring slice are free.
		/// Host-side wait: parks the thread, no queue round-trip.
		if (wait_value != 0)
		{
			EMBER_PROFILE_SCOPE_C("gpu: wait_frame", PROFILE_COLOR_WAIT);

			VkSemaphoreWaitInfo wait_info{
				.sType			= VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
				.semaphoreCount = 1,
				.pSemaphores	= &m_backend->timeline,
				.pValues		= &wait_value,
			};

			vk::note_result(*m_backend, vkWaitSemaphores(m_backend->device, &wait_info, UINT64_MAX));
		}

		EMBER_VK_CHECK(vkResetCommandPool(m_backend->device, m_backend->slots[slot].pool, 0));

		// The wait above proved wait_value completed; the graveyard rides the frame pacing
		// and needs no extra queries.
		vk::drain_deferred_destroys(*m_backend, wait_value);
		m_backend->pending_present_count = 0;
		m_backend->frame_open			 = true;

		return {.frame_index = static_cast<u32>(m_backend->frame_index), .slot = slot};
	}

	void Device::end_frame() noexcept
	{
		if (m_backend == nullptr)
			return;

		vk::DeviceBackend& backend = *m_backend;

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());
		EMBER_ASSERT(backend.frame_open && "end_frame without begin_frame");

		const u32 slot	= static_cast<u32>(backend.frame_index % backend.frames_in_flight);
		const u64 value = ++backend.timeline_value;

		VkCommandBuffer cmd = backend.slots[slot].commands;

		const VkCommandBufferBeginInfo begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		EMBER_VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

		// Placeholder visual until CommandList lands: clear every acquired backbuffer with a
		// slowly cycling hue. Animated on purpose — a static clear can't prove frame pacing.
		const f32 t = static_cast<f32>(backend.frame_index) * 0.02f;
		const VkClearColorValue clear{
			.float32 = {
				0.5f + 0.5f * std::sin(t),
				0.5f + 0.5f * std::sin(t + 2.09f),
				0.5f + 0.5f * std::sin(t + 4.19f),
				1.0f,
			}};

		for (u32 i = 0; i < backend.pending_present_count; ++i)
		{
			const vk::SwapchainData& data = backend.resources.swapchains.get(backend.pending_presents[i].swapchain);
			const vk::TextureHot& hot	  = backend.resources.textures.get(data.images[data.acquired_image]);

			// Acquired contents are undefined; the acquire-semaphore wait below is scoped to
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

		EMBER_VK_CHECK(vkEndCommandBuffer(cmd));

		// One submit: wait every acquire semaphore, run the frame's commands, signal the
		// timeline (CPU pacing) and every acquired image's present semaphore (WSI).
		VkSemaphoreSubmitInfo waits[MAX_SWAPCHAINS];
		VkSemaphoreSubmitInfo signals[MAX_SWAPCHAINS + 1];
		u32 wait_count	 = 0;
		u32 signal_count = 0;

		for (u32 i = 0; i < backend.pending_present_count; ++i)
		{
			const vk::SwapchainData& data = backend.resources.swapchains.get(backend.pending_presents[i].swapchain);

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
			.semaphore = backend.timeline,
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

		vk::note_result(backend, vkQueueSubmit2(backend.graphics.handle, 1, &submit_info, VK_NULL_HANDLE));

		// Batched present: every swapchain touched this frame in one call, results per entry.
		if (backend.pending_present_count > 0)
		{
			VkSwapchainKHR swapchains[MAX_SWAPCHAINS];
			VkSemaphore present_waits[MAX_SWAPCHAINS];
			u32 image_indices[MAX_SWAPCHAINS];
			VkResult results[MAX_SWAPCHAINS];

			for (u32 i = 0; i < backend.pending_present_count; ++i)
			{
				const vk::SwapchainData& data = backend.resources.swapchains.get(backend.pending_presents[i].swapchain);

				swapchains[i]	 = data.swapchain;
				present_waits[i] = data.present_semaphores[data.acquired_image];
				image_indices[i] = backend.pending_presents[i].image_index;
			}

			const VkPresentInfoKHR present_info{
				.sType				= VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
				.waitSemaphoreCount = backend.pending_present_count,
				.pWaitSemaphores	= present_waits,
				.swapchainCount		= backend.pending_present_count,
				.pSwapchains		= swapchains,
				.pImageIndices		= image_indices,
				.pResults			= results,
			};

			vk::note_result(backend, vkQueuePresentKHR(backend.graphics.handle, &present_info));

			// Per-swapchain outcome: OUT_OF_DATE/SUBOPTIMAL here means recreate at next acquire.
			for (u32 i = 0; i < backend.pending_present_count; ++i)
				if (results[i] == VK_ERROR_OUT_OF_DATE_KHR || results[i] == VK_SUBOPTIMAL_KHR)
					backend.resources.swapchains.get(backend.pending_presents[i].swapchain).needs_recreate = true;
		}

		backend.slots[slot].submitted = value;
		backend.frame_open			  = false;
		++backend.frame_index;
	}

	SwapchainHandle Device::create_swapchain(const SwapchainDef& def) noexcept
	{
		if (m_backend == nullptr)
			return {};

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());

		if (m_backend->platform == nullptr)
		{
			EMBER_ERROR("gpu: cannot create a swapchain on a headless device");
			return {};
		}

		SwapchainHandle handle = m_backend->resources.swapchains.emplace();

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: swapchain pool exhausted");
			return {};
		}

		if (!vk::swapchain_create(*m_backend, def, m_backend->resources.swapchains.get(handle)))
		{
			(void)m_backend->resources.swapchains.erase(handle);
			return {};
		}

		return handle;
	}

	void Device::destroy(SwapchainHandle handle) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());

		if (vk::SwapchainData* data = m_backend->resources.swapchains.try_get(handle))
		{
			// Acquired-then-destroyed this frame: drop the pending present so end_frame
			// never walks a dead handle. The acquired image is simply never presented.
			vk::PendingPresent* pending = m_backend->pending_presents.data();
			u32& count					= m_backend->pending_present_count;

			for (u32 i = 0; i < count;)
			{
				if (pending[i].swapchain == handle)
					pending[i] = pending[--count]; // unordered remove: batch order carries no meaning
				else
					++i;
			}

			vk::swapchain_destroy(*m_backend, *data);
			(void)m_backend->resources.swapchains.erase(handle);
		}
	}

	TextureHandle Device::acquire(SwapchainHandle handle) noexcept
	{
		if (m_backend == nullptr)
			return {};

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());
		EMBER_ASSERT(m_backend->frame_open && "acquire outside begin_frame/end_frame");

		if (!m_backend->resources.swapchains.contains(handle))
		{
			EMBER_ASSERT(false && "Invalid SwapchainHandle");
			return {};
		}

		return vk::swapchain_acquire(*m_backend, handle);
	}

	Extent2D Device::swapchain_extent(SwapchainHandle handle) const noexcept
	{
		if (m_backend == nullptr)
			return {};

		if (auto* data = m_backend->resources.swapchains.try_get(handle))
			return {data->extent.width, data->extent.height};

		return {};
	}

	BufferHandle Device::create_buffer(const BufferDef& def) noexcept
	{
		if (m_backend == nullptr)
			return {};

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());
		return vk::create_buffer(*m_backend, def);
	}

	void Device::destroy(BufferHandle handle) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());
		vk::destroy_buffer(*m_backend, handle);
	}

	bool Device::is_valid(BufferHandle handle) const noexcept
	{
		return m_backend != nullptr && m_backend->resources.buffers.contains(handle);
	}

	void* Device::mapped(BufferHandle handle) noexcept
	{
		if (m_backend == nullptr)
			return nullptr;

		if (auto* cold = m_backend->resources.buffers.try_get_cold(handle))
			return cold->mapped;

		return nullptr;
	}
}
