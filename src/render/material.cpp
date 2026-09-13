#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/memory/pmr/block_allocator.h>
#include <ember/render/gpu_scene.h>
#include <ember/render/material.h>

#include <algorithm>
#include <cstring>

namespace ember::render
{
	MaterialPool::MaterialPool() noexcept : m_records(MemoryTag::Graphics), m_dirty(MemoryTag::Graphics) {}

	MaterialPool::~MaterialPool() noexcept
	{
		if (m_block != nullptr)
			m_resource->deallocate(m_block, m_block_size, alignof(u64));
	}

	void MaterialPool::init(gpu::Device& device, const MaterialPoolDef& def) noexcept
	{
		EMBER_ASSERT(m_table.is_null() && "init runs once");
		EMBER_ASSERT(ember::render::is_valid(def));

		m_records.init(def.capacity);
		m_stride = def.stride;

		m_table = device.create_buffer({
			.name  = def.name,
			.size  = u64{def.capacity} * def.stride,
			.usage = gpu::BufferUsage::Storage,
		});

		if (m_table.is_null())
		{
			EMBER_ERROR("material pool '{}' creation failed", def.name);
			return;
		}

		m_resource	 = &memory::heap(MemoryTag::Graphics);
		m_block_size = u64{def.capacity} * def.stride;
		m_block		 = m_resource->allocate(m_block_size, alignof(u64));
		m_shadow	 = static_cast<u8*>(m_block);

		m_dirty.init(def.capacity);

		// The error record claims slot 0 for the pool's lifetime, mirroring the
		// bindless heap's fallback convention: a null handle's index lands here.
		m_error = m_records.insert(u8{0});
		EMBER_ASSERT(m_error.index == 0);

		std::memcpy(m_shadow, def.error_record.data(), m_stride);
		m_dirty.mark(0);
	}

	void MaterialPool::shutdown(gpu::Device& device) noexcept
	{
		if (!m_error.is_null())
		{
			const bool erased = m_records.erase(m_error);
			EMBER_ASSERT(erased);
			(void)erased;
			m_error = {};
		}

		EMBER_ASSERT(m_records.empty() && "destroy materials before pool shutdown");

		if (!m_table.is_null())
			device.destroy(m_table);

		m_table = {};
	}

	MaterialHandle MaterialPool::create(Span<const u8> data) noexcept
	{
		EMBER_ASSERT(data.size() == m_stride && "one record of exactly stride bytes");
		if (m_table.is_null() || data.size() != m_stride) [[unlikely]]
			return {};

		const MaterialHandle handle = m_records.insert(u8{0});

		if (handle.is_null()) [[unlikely]]
		{
			EMBER_ERROR("material pool is full ({})", m_records.capacity());
			return handle;
		}

		std::memcpy(m_shadow + u64{handle.index} * m_stride, data.data(), m_stride);
		m_dirty.mark(handle.index);

		return handle;
	}

	void MaterialPool::update(MaterialHandle handle, Span<const u8> data) noexcept
	{
		EMBER_ASSERT(data.size() == m_stride && "one record of exactly stride bytes");

		if (m_records.get(handle) == nullptr || data.size() != m_stride) [[unlikely]]
			return;

		std::memcpy(m_shadow + u64{handle.index} * m_stride, data.data(), m_stride);
		m_dirty.mark(handle.index);
	}

	void MaterialPool::destroy(MaterialHandle handle) noexcept
	{
		EMBER_ASSERT(handle != m_error);

		if (m_records.get(handle) == nullptr)
			return;

		// The error record covers the slot until reuse, so anything still
		// pointing here shows the family's mistake look instead of recycled
		// bytes. Lifetime stays the caller's contract.
		std::memcpy(m_shadow + u64{handle.index} * m_stride, m_shadow, m_stride);
		m_dirty.mark(handle.index);

		const bool erased = m_records.erase(handle);
		EMBER_ASSERT(erased);
		(void)erased;
	}

	void MaterialPool::sync(gpu::Device& device) noexcept
	{
		const Span<const u32> dirty = m_dirty.slots();
		if (dirty.empty())
			return;

		// Writers append in whatever order they finished, so the sort happens on a frame copy;
		// the set itself stays read only for consumers.
		const u32 count = static_cast<u32>(dirty.size());
		auto* slots		= static_cast<u32*>(memory::frame_memory().allocate_fast(count * sizeof(u32), alignof(u32)));

		std::memcpy(slots, dirty.data(), count * sizeof(u32));
		std::sort(slots, slots + count);

		// Dedup already happened at mark, so each slot uploads exactly once
		// with its final shadow bytes: last write wins, and a destroyed then
		// reused slot lands as the new record.
		for_each_slot_run({slots, count},
						  [&](u32 first, u32 run) noexcept
						  {
							  device.update_buffer(m_table, u64{first} * m_stride,
												   {m_shadow + u64{first} * m_stride, u64{run} * m_stride});
						  });

		m_dirty.clear();
	}
}
