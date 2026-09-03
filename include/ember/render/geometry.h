#pragma once

#include <ember/containers/pool.h>
#include <ember/containers/range_allocator.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/buffer.h>
#include <ember/gpu/common.h>
#include <ember/render/common.h>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <type_traits>

namespace ember::gpu
{
	class Device;
}

namespace ember::render
{
	/// Per-vertex attribute stream: octahedral normal, rgba8 color, uv. Positions
	/// travel separately as float4 so depth-only passes pull a tight stream.
	struct AttributeData
	{
		u32 normal	 = 0;
		u32 color	 = 0xFFFFFFFF;
		glm::vec2 uv = {};
	};

	/**
	 * One geometry as GPU passes see it, mirrored in shaders. Indices are stored
	 * rebased to pool-global vertex ids, so draws never carry a base vertex and
	 * SV_VertexID addresses the shared streams directly; first_vertex and vertex_count
	 * exist for range frees, validation and stats.
	 */
	struct GeometryData
	{
		u32 first_index	 = 0;
		u32 index_count	 = 0;
		u32 first_vertex = 0;
		u32 vertex_count = 0;
		glm::vec4 sphere = {}; // authoring space center and radius
	};

	static_assert(sizeof(AttributeData) == 16 && std::is_trivially_copyable_v<AttributeData>);
	static_assert(sizeof(GeometryData) == 32 && std::is_trivially_copyable_v<GeometryData>);

	struct GeometryDef
	{
		const char* name = "geometry";

		Span<const u8> positions  = {}; // float4 per vertex
		Span<const u8> attributes = {}; // AttributeData per vertex
		Span<const u32> indices	  = {}; // local, rebased at upload

		glm::vec4 sphere = {0.0f, 0.0f, 0.0f, 1.0f};
	};

	[[nodiscard]] constexpr bool is_valid(const GeometryDef& def) noexcept
	{
		if (def.name == nullptr || def.positions.empty() || def.indices.empty())
			return false;

		if (def.positions.size() % 16 != 0)
			return false;

		// The streams are parallel: one vertex id addresses both.
		if (def.attributes.size() != (def.positions.size() / 16) * sizeof(AttributeData))
			return false;

		return def.sphere.w >= 0.0f;
	}

	/// Element capacities. Defaults hold a cozy scene; size to the game at init.
	struct GeometryPoolDef
	{
		u32 max_geometries	= 4096;
		u32 vertex_capacity = 1u << 20;
		u32 index_capacity	= 1u << 22;
	};

	[[nodiscard]] constexpr bool is_valid(const GeometryPoolDef& def) noexcept
	{
		return def.max_geometries != 0 && def.max_geometries <= 65536 && def.vertex_capacity != 0 &&
			   def.index_capacity != 0;
	}

	/**
	 * Persistent mesh storage: every geometry lives in three shared device
	 * buffers (positions, attributes, indices) plus a GeometryData table, all
	 * bindless. Draws differ only by offsets, which is what lets one index
	 * buffer bind serve every mesh draw and lets GPU culling write indirect
	 * arguments from the table alone.
	 *
	 * Geometries are immutable: create uploads everything, destroy frees the
	 * ranges and scrubs the table record to index_count zero so a stale object
	 * reference degenerates to an empty draw. Mutation is create plus destroy.
	 *
	 * Uploads and table scrubs ride the staging ring, whose batch entry barrier
	 * orders all prior GPU work before the copies. That barrier is what makes
	 * immediate range reuse safe while old frames are still in flight.
	 *
	 * Not thread-safe; create and destroy belong to the owner thread.
	 */
	class GeometryPool
	{
	public:
		GeometryPool() noexcept;

		GeometryPool(const GeometryPool&)			 = delete;
		GeometryPool& operator=(const GeometryPool&) = delete;

		/// Creates the shared buffers. Runs once, before any create.
		void init(gpu::Device& device, const GeometryPoolDef& def) noexcept;

		/// Destroys the shared buffers. Call before the device goes down; live
		/// geometries at this point are a leak and assert in debug.
		void shutdown(gpu::Device& device) noexcept;

		/// Null handle when validation, space or slots fail; the failure is logged.
		[[nodiscard]] GeometryHandle create(gpu::Device& device, const GeometryDef& def) noexcept;

		/// Safe on null and stale handles.
		void destroy(gpu::Device& device, GeometryHandle handle) noexcept;

		/// Weak deref: nullptr when the handle is stale or null.
		[[nodiscard]] const GeometryData* get(GeometryHandle handle) const noexcept;

		/// Bound once per pass; every geometry draws from it by offset.
		[[nodiscard]] BufferHandle index_buffer() const noexcept { return m_indices; }

		/// Bindless slots for shaders: the two vertex streams and the table.
		[[nodiscard]] u32 positions_index() const noexcept { return bindless_index(m_positions); }
		[[nodiscard]] u32 attributes_index() const noexcept { return bindless_index(m_attributes); }
		[[nodiscard]] u32 table_index() const noexcept { return bindless_index(m_table); }

		[[nodiscard]] u32 geometry_count() const noexcept { return m_records.size(); }
		[[nodiscard]] u32 vertex_used() const noexcept { return m_vertex_space.used(); }
		[[nodiscard]] u32 index_used() const noexcept { return m_index_space.used(); }

	private:
		Pool<Geometry, GeometryData> m_records;
		RangeAllocator m_vertex_space;
		RangeAllocator m_index_space;

		BufferHandle m_positions  = {};
		BufferHandle m_attributes = {};
		BufferHandle m_indices	  = {};
		BufferHandle m_table	  = {};
	};
}
