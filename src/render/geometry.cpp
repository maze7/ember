#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/memory/pmr/arena_resource.h>
#include <ember/render/geometry.h>

namespace ember::render
{
	GeometryPool::GeometryPool() noexcept
		: m_records(MemoryTag::Graphics), m_vertex_space(MemoryTag::Graphics), m_index_space(MemoryTag::Graphics)
	{
	}

	void GeometryPool::init(gpu::Device& device, const GeometryPoolDef& def) noexcept
	{
		EMBER_ASSERT(m_positions.is_null() && "init runs once");
		EMBER_ASSERT(is_valid(def));

		m_records.init(def.max_geometries);
		m_vertex_space.init(def.vertex_capacity);
		m_index_space.init(def.index_capacity);

		m_positions = device.create_buffer({
			.name  = "geometry.positions",
			.size  = u64{def.vertex_capacity} * 16,
			.usage = gpu::BufferUsage::Storage,
		});

		m_attributes = device.create_buffer({
			.name  = "geometry.attributes",
			.size  = u64{def.vertex_capacity} * sizeof(AttributeData),
			.usage = gpu::BufferUsage::Storage,
		});

		m_indices = device.create_buffer({
			.name  = "geometry.indices",
			.size  = u64{def.index_capacity} * sizeof(u32),
			.usage = gpu::BufferUsage::Index,
		});

		m_table = device.create_buffer({
			.name  = "geometry.table",
			.size  = u64{def.max_geometries} * sizeof(GeometryData),
			.usage = gpu::BufferUsage::Storage,
		});

		if (m_positions.is_null() || m_attributes.is_null() || m_indices.is_null() || m_table.is_null())
		{
			EMBER_ERROR("geometry pool buffer creation failed");
			shutdown(device);
		}
	}

	void GeometryPool::shutdown(gpu::Device& device) noexcept
	{
		EMBER_ASSERT(m_records.empty() && "destroy geometries before pool shutdown");

		if (!m_positions.is_null())
			device.destroy(m_positions);
		if (!m_attributes.is_null())
			device.destroy(m_attributes);
		if (!m_indices.is_null())
			device.destroy(m_indices);
		if (!m_table.is_null())
			device.destroy(m_table);

		m_positions	 = {};
		m_attributes = {};
		m_indices	 = {};
		m_table		 = {};
	}

	GeometryHandle GeometryPool::create(gpu::Device& device, const GeometryDef& def) noexcept
	{
		EMBER_ASSERT(is_valid(def));
		if (!is_valid(def) || m_positions.is_null()) [[unlikely]]
			return {};

		const u32 vertex_count = static_cast<u32>(def.positions.size() / 16);
		const u32 index_count  = static_cast<u32>(def.indices.size());

		const u32 first_vertex = m_vertex_space.allocate(vertex_count);
		if (first_vertex == RangeAllocator::INVALID_OFFSET) [[unlikely]]
		{
			EMBER_ERROR("geometry '{}': vertex space exhausted ({} in use)", def.name, m_vertex_space.used());
			return {};
		}

		const u32 first_index = m_index_space.allocate(index_count);
		if (first_index == RangeAllocator::INVALID_OFFSET) [[unlikely]]
		{
			EMBER_ERROR("geometry '{}': index space exhausted ({} in use)", def.name, m_index_space.used());
			m_vertex_space.free(first_vertex, vertex_count);
			return {};
		}

		const GeometryHandle handle = m_records.insert(
			GeometryData{
				.first_index  = first_index,
				.index_count  = index_count,
				.first_vertex = first_vertex,
				.vertex_count = vertex_count,
				.sphere		  = def.sphere,
			});

		if (handle.is_null()) [[unlikely]]
		{
			EMBER_ERROR("geometry '{}': record pool is full ({})", def.name, m_records.capacity());
			m_index_space.free(first_index, index_count);
			m_vertex_space.free(first_vertex, vertex_count);
			return {};
		}

		device.update_buffer(m_positions, u64{first_vertex} * 16, def.positions);
		device.update_buffer(m_attributes, u64{first_vertex} * sizeof(AttributeData), def.attributes);

		// Rebase to pool-global vertex ids so draws carry no base vertex and
		// SV_VertexID addresses the shared streams directly. The scratch only
		// feeds the staging copy inside update_buffer, so frame arena lifetime
		// is enough even during boot.
		u32* rebased = static_cast<u32*>(memory::frame_arena().allocate_fast(index_count * sizeof(u32), alignof(u32)));

		for (u32 i = 0; i < index_count; ++i)
		{
			EMBER_ASSERT(def.indices[i] < vertex_count && "index outside its vertex range");
			rebased[i] = def.indices[i] + first_vertex;
		}

		device.update_buffer(
			m_indices,
			u64{first_index} * sizeof(u32),
			{reinterpret_cast<const u8*>(rebased), index_count * sizeof(u32)});

		const GeometryData* record = m_records.get(handle);
		device.update_buffer(
			m_table,
			u64{handle.index} * sizeof(GeometryData),
			{reinterpret_cast<const u8*>(record), sizeof(GeometryData)});

		return handle;
	}

	void GeometryPool::destroy(gpu::Device& device, GeometryHandle handle) noexcept
	{
		const GeometryData* record = m_records.get(handle);
		if (record == nullptr)
			return;

		m_vertex_space.free(record->first_vertex, record->vertex_count);
		m_index_space.free(record->first_index, record->index_count);

		// Scrub the table so an object still referencing this slot culls to an
		// empty draw instead of reading recycled ranges.
		const GeometryData dead{};
		device.update_buffer(
			m_table,
			u64{handle.index} * sizeof(GeometryData),
			{reinterpret_cast<const u8*>(&dead), sizeof(GeometryData)});

		const bool erased = m_records.erase(handle);
		EMBER_ASSERT(erased);
		(void)erased;
	}

	const GeometryData* GeometryPool::get(GeometryHandle handle) const noexcept { return m_records.get(handle); }
}
