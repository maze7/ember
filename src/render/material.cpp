#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/render/gpu_scene.h>
#include <ember/render/material.h>

#include <algorithm>
#include <cstring>

namespace ember::render
{
	MaterialPool::MaterialPool() noexcept : m_records(MemoryTag::Graphics) {}

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

		const size_t bits_bytes = ((def.capacity + 63) / 64) * sizeof(u64);
		const size_t list_bytes = def.capacity * sizeof(u32);

		m_resource	 = &memory::heap(MemoryTag::Graphics);
		m_block_size = bits_bytes + list_bytes + u64{def.capacity} * def.stride;
		m_block		 = m_resource->allocate(m_block_size, alignof(u64));

		auto* base	 = static_cast<std::byte*>(m_block);
		m_dirty_bits = reinterpret_cast<u64*>(base);
		m_dirty_list = reinterpret_cast<u32*>(base + bits_bytes);
		m_shadow	 = reinterpret_cast<u8*>(base + bits_bytes + list_bytes);

		std::memset(m_dirty_bits, 0, bits_bytes);

		// The error record claims slot 0 for the pool's lifetime, mirroring the
		// bindless heap's fallback convention: a null handle's index lands here.
		m_error = m_records.insert(u8{0});
		EMBER_ASSERT(m_error.index == 0);

		std::memcpy(m_shadow, def.error_record.data(), m_stride);
		mark_dirty(0);
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
		mark_dirty(handle.index);

		return handle;
	}

	void MaterialPool::update(MaterialHandle handle, Span<const u8> data) noexcept
	{
		EMBER_ASSERT(data.size() == m_stride && "one record of exactly stride bytes");

		if (m_records.get(handle) == nullptr || data.size() != m_stride) [[unlikely]]
			return;

		std::memcpy(m_shadow + u64{handle.index} * m_stride, data.data(), m_stride);
		mark_dirty(handle.index);
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
		mark_dirty(handle.index);

		const bool erased = m_records.erase(handle);
		EMBER_ASSERT(erased);
		(void)erased;
	}

	void MaterialPool::sync(gpu::Device& device) noexcept
	{
		if (m_dirty_count == 0)
			return;

		// Dedup already happened at mark_dirty, so each slot uploads exactly
		// once with its final shadow bytes: last write wins, and a destroyed
		// then reused slot lands as the new record.
		std::sort(m_dirty_list, m_dirty_list + m_dirty_count);

		for_each_slot_run(
			{m_dirty_list, m_dirty_count},
			[&](u32 first, u32 run) noexcept
			{
				device.update_buffer(
					m_table, u64{first} * m_stride, {m_shadow + u64{first} * m_stride, u64{run} * m_stride});
			});

		for (u32 i = 0; i < m_dirty_count; ++i)
			m_dirty_bits[m_dirty_list[i] >> 6] &= ~(u64{1} << (m_dirty_list[i] & 63));

		m_dirty_count = 0;
	}

	void MaterialPool::mark_dirty(u32 slot) noexcept
	{
		u64& word	  = m_dirty_bits[slot >> 6];
		const u64 bit = u64{1} << (slot & 63);

		if ((word & bit) != 0)
			return;

		word						  |= bit;
		m_dirty_list[m_dirty_count++]  = slot;
	}
}
