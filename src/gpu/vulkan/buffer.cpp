#include "ember/gpu/common.h"
#include <ember/gpu/buffer.h>
#include <gpu/vulkan/backend.h>

#include <cstring>
#include <vulkan/vulkan_core.h>

namespace ember::gpu::vk
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

	BufferHandle create_buffer(DeviceBackend& backend, const BufferDef& def) noexcept
	{
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
		if (backend.buffer_device_address)
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

		if (auto vr = vmaCreateBuffer(backend.allocator, &buffer_info, &alloc_info, &buffer, &allocation, &result);
			vr != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: buffer '{}' ({} bytes) failed: {}", def.name, def.size, result_name(vr));
			return {};
		}

		VkDeviceAddress address = 0;
		if (backend.buffer_device_address)
		{
			VkBufferDeviceAddressInfo address_info{
				.sType	= VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
				.buffer = buffer,
			};

			address = vkGetBufferDeviceAddress(backend.device, &address_info);
		}

		BufferHandle handle = backend.resources.buffers.insert(
			BufferHot{.handle = buffer, .address = address},
			BufferCold{.allocation = allocation, .size = def.size, .mapped = result.pMappedData});

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: buffer pool exhausted ({})", def.name);
			vmaDestroyBuffer(backend.allocator, buffer, allocation);
			return {};
		}

		set_name(backend, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(buffer), def.name);

		if (!def.initial_data.empty())
		{
			EMBER_ASSERT(def.initial_data.size() <= def.size);

			if (result.pMappedData != nullptr)
			{
				// Mapped memory: write it directly, flush for non-coherent heaps
				// (a no-op on coherent ones, i.e. all desktop in practice).
				memcpy(result.pMappedData, def.initial_data.data(), def.initial_data.size());
				(void)vmaFlushAllocation(backend.allocator, allocation, 0, def.initial_data.size());
			}
			else
			{
				// TODO: Cal
				// update_buffer(backend, handle, 0, def.initial_data);
			}
		}

		return handle;
	}

	void destroy_buffer(DeviceBackend& backend, BufferHandle handle) noexcept
	{
		BufferHot* hot = backend.resources.buffers.try_get(handle);
		if (hot == nullptr)
			return;

		// Erase-then-defer: the handle (and its bindless slot) dies immediately; the
		// native object outlives every frame that can reference it.
		defer_destroy(backend, hot->handle, backend.resources.buffers.get_cold(handle).allocation);
		(void)backend.resources.buffers.erase(handle);
	}
}
