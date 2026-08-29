#include <ember/gpu/buffer.h>
#include <ember/gpu/common.h>
#include <gpu/vulkan/backend.h>

#include <cstring>

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
		EMBER_GPU_GUARD({});

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

		// bufferDeviceAddress is in REQUIRED_12 (GPU-driven vertex pulling and
		// draw-record buffers), so every buffer gets an address unconditionally.
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

		if (auto vr =
				vmaCreateBuffer(m_backend->context.allocator, &buffer_info, &alloc_info, &buffer, &allocation, &result);
			vr != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: buffer '{}' ({} bytes) failed: {}", def.name, def.size, vk::result_name(vr));
			return {};
		}

		const VkBufferDeviceAddressInfo address_info{
			.sType	= VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = buffer,
		};

		const VkDeviceAddress address = vkGetBufferDeviceAddress(m_backend->context.device, &address_info);

		BufferHandle handle = m_backend->resources.buffers.insert(
			vk::BufferHot{.handle = buffer, .address = address},
			vk::BufferCold{.allocation = allocation, .size = def.size, .mapped = result.pMappedData});

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: buffer pool exhausted ({})", def.name);
			vmaDestroyBuffer(m_backend->context.allocator, buffer, allocation);
			return {};
		}

		vk::set_name(m_backend->context, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(buffer), def.name);

		// Storage buffers live in the bindless SSBO array at their pool index.
		if ((def.usage & BufferUsage::Storage) != BufferUsage::None)
			m_backend->descriptor_heap.write_buffer(m_backend->context, handle.index, buffer, def.size);

		if (!def.initial_data.empty())
		{
			EMBER_ASSERT(def.initial_data.size() <= def.size);

			if (result.pMappedData != nullptr)
			{
				// Mapped memory: write it directly, flush for non-coherent heaps
				// (a no-op on coherent ones, i.e. all desktop in practice).
				memcpy(result.pMappedData, def.initial_data.data(), def.initial_data.size());
				(void)vmaFlushAllocation(m_backend->context.allocator, allocation, 0, def.initial_data.size());
			}
			else
			{
				// DeviceLocal: staged, TRANSFER_DST is provided to every buffer.
				vk::staging_upload(*m_backend, buffer, 0, def.initial_data);
			}
		}

		return handle;
	}

	void Device::destroy(BufferHandle handle) noexcept
	{
		EMBER_GPU_GUARD();

		vk::BufferHot* hot = m_backend->resources.buffers.get(handle);
		if (hot == nullptr)
			return;

		// Retire-then-defer: the handle dies immediately; the native object and the
		// slot's reuse wait for every frame that can still reference the index.
		m_backend->destroy_queue.destroy(hot->handle, m_backend->resources.buffers.get_cold(handle)->allocation);
		m_backend->destroy_queue.reset_slot(handle.index, vk::HeapArray::Buffer);
		(void)m_backend->resources.buffers.retire(handle);
	}

	bool Device::is_valid(BufferHandle handle) const noexcept
	{
		return m_backend != nullptr && m_backend->resources.buffers.contains(handle);
	}

	void* Device::mapped(BufferHandle handle) noexcept
	{
		if (m_backend == nullptr)
			return nullptr;

		if (auto* cold = m_backend->resources.buffers.get_cold(handle))
			return cold->mapped;

		return nullptr;
	}

	void Device::update_buffer(BufferHandle handle, u64 offset, Span<const u8> data) noexcept
	{
		EMBER_GPU_GUARD();

		if (data.empty())
			return;

		const vk::BufferHot* hot = m_backend->resources.buffers.get(handle);

		if (hot == nullptr)
		{
			// Unlike destroy (idempotence is a feature there), updating a dead handle is
			// always a bug upstream: the data was meant for something.
			EMBER_ASSERT(false && "update_buffer on a stale handle");
			return;
		}

		const vk::BufferCold& cold = *m_backend->resources.buffers.get_cold(handle);
		EMBER_ASSERT(offset + data.size() <= cold.size);

		if (cold.mapped != nullptr)
		{
			// Upload/Readback: straight through the mapping. GPU-side hazards are the
			// caller's contract (per-frame versioning); the GPU never copies here at all.
			std::memcpy(static_cast<u8*>(cold.mapped) + offset, data.data(), data.size());
			(void)vmaFlushAllocation(m_backend->context.allocator, cold.allocation, offset, data.size());
			return;
		}

		vk::staging_upload(*m_backend, hot->handle, offset, data);
	}

	u64 Device::buffer_address(BufferHandle handle) const noexcept
	{
		if (m_backend == nullptr)
			return 0;

		if (const vk::BufferHot* hot = m_backend->resources.buffers.get(handle))
			return hot->address;

		// Like update_buffer: asking for a dead buffer's address is always a bug
		// upstream, and asserting here beats a GPU fault three frames later.
		EMBER_ASSERT(false && "buffer_address on a stale handle");
		return 0;
	}
}
