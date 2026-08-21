#include "ember/sync/thread.h"
#include <ember/core/profile.h>
#include <ember/gpu/device.h>
#include <gpu/vulkan/backend.h>
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

		bool expected = false;
		if (!s_device_claimed.compare_exchange_weak(
				expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
		{
			EMBER_ERROR("gpu: only one Device may exist at a time");
			return;
		}

		DeviceBackend* backend = memory::new_object<DeviceBackend>(MemoryTag::Graphics);
		backend->platform	   = def.platform;
		backend->resources.reserve(def.limits);

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

		// TODO(resources slice): leak-report and destroy surviving pooled natives here.

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

		m_backend->frame_open = true;

		return {.frame_index = static_cast<u32>(m_backend->frame_index), .slot = slot};
	}

	void Device::end_frame() noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());
		EMBER_ASSERT(m_backend->frame_open && "end_frame without begin_frame");

		u32 slot  = static_cast<u32>(m_backend->frame_index % m_backend->frames_in_flight);
		u64 value = ++m_backend->timeline_value;

		// Empty submit that signals the frame's timeline value (sync2 allows zero command buffers).
		VkSemaphoreSubmitInfo signal_info{
			.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = m_backend->timeline,
			.value	   = value,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		VkSubmitInfo2 submit_info{
			.sType					  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.signalSemaphoreInfoCount = 1,
			.pSignalSemaphoreInfos	  = &signal_info,
		};

		vk::note_result(*m_backend, vkQueueSubmit2(m_backend->graphics.handle, 1, &submit_info, VK_NULL_HANDLE));

		m_backend->slots[slot].submitted = value;
		m_backend->frame_open			 = false;
		++m_backend->frame_index;
	}
}
