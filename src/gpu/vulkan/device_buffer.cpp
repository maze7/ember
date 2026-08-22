#include "ember/gpu/common.h"
#include <ember/gpu/buffer.h>
#include <gpu/vulkan/device_state.h>

#include <cstring>
#include <vulkan/vulkan_core.h>

namespace ember::gpu
{
	namespace
	{
		[[nodiscard]] VkBufferUsageFlags to_vk_usage(BufferUsage usage, MemoryLocation memory) noexcept
		{
			VkBufferUsageFlags flags = 0;

			if ((usage & BufferUsage::Vertex) != BufferUsage::None)
				flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			if ((usage & BufferUsage::Index) != BufferUsage::None)
				flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
			if ((usage & BufferUsage::Constant) != BufferUsage::None)
				flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			if ((usage & BufferUsage::Storage) != BufferUsage::None)
				flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			if ((usage & BufferUsage::Indirect) != BufferUsage::None)
				flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
			if ((usage & BufferUsage::CopySrc) != BufferUsage::None)
				flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

			// Buffers don't pay for usage bits the way images do (no layout/compression consequences on
			// any of our target hardware), so we can be generous where it buys API guarantees: every
			// DeviceLocal buffer accepts update_buffer.
			if ((usage & BufferUsage::CopyDst) != BufferUsage::None || memory == MemoryLocation::DeviceLocal)
				flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

			return flags;
		}
	}

	BufferHandle Device::create_buffer(const BufferDef& def) noexcept
	{
		if (m_backend == nullptr)
			return {};

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());

		if (def.size == 0)
		{
			EMBER_ERROR("gpu: buffer '{}' has zero size", def.name);
			return {};
		}

		VkBufferCreateInfo buffer_info{
			.sType		 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size		 = def.size,
			.usage		 = to_vk_usage(def.usage, def.memory),
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};

		// Free while the feature is present, and it is what makes GPU-driven vertex pulling
		// and draw-record buffers possible.
		if (m_backend->buffer_device_address)
			buffer_info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		VmaAllocationCreateInfo alloc_info{};
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		switch (def.memory)
		{
			case MemoryLocation::DeviceLocal:
				break;
			case MemoryLocation::Upload:
				alloc_info.flags =
					VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
				alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
				break;
			case MemoryLocation::Readback:
				alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
				alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
				break;
		}

		VkBuffer buffer			 = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo result{};

		if (auto vr = vmaCreateBuffer(m_backend->allocator, &buffer_info, &alloc_info, &buffer, &allocation, &result);
			vr != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: buffer '{}' ({} bytes) failed: {}", def.name, def.size, vk::result_name(vr));
			return {};
		}

		VkDeviceAddress address = 0;
		if (m_backend->buffer_device_address)
		{
			VkBufferDeviceAddressInfo address_info{
				.sType	= VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
				.buffer = buffer,
			};

			address = vkGetBufferDeviceAddress(m_backend->device, &address_info);
		}

		BufferHandle handle = m_backend->resources.buffers.insert(
			vk::BufferHot{.handle = buffer, .address = address},
			vk::BufferCold{.allocation = allocation, .size = def.size, .mapped = result.pMappedData});

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: buffer pool exhausted ({})", def.name);
			vmaDestroyBuffer(m_backend->allocator, buffer, allocation);
			return {};
		}

		vk::set_name(*m_backend, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(buffer), def.name);

		if (!def.initial_data.empty())
		{
			EMBER_ASSERT(def.initial_data.size() <= def.size);

			if (result.pMappedData != nullptr)
			{
				// Mapped memory: write it directly, flush for non-coherent heaps
				// (a no-op on coherent ones, i.e. all desktop in practice).
				memcpy(result.pMappedData, def.initial_data.data(), def.initial_data.size());
				(void)vmaFlushAllocation(m_backend->allocator, allocation, 0, def.initial_data.size());
			}
			else
			{
				// TODO: Cal
				// update_buffer(backend, handle, 0, def.initial_data);
			}
		}

		return handle;
	}

	void Device::destroy(BufferHandle handle) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_backend->owner_thread == current_thread_id());

		vk::BufferHot* hot = m_backend->resources.buffers.try_get(handle);
		if (hot == nullptr)
			return;

		// Erase-then-defer: the handle (and its bindless slot) dies immediately; the
		// native object outlives every frame that can reference it.
		vk::defer_destroy(*m_backend, hot->handle, m_backend->resources.buffers.get_cold(handle).allocation);
		(void)m_backend->resources.buffers.erase(handle);
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
