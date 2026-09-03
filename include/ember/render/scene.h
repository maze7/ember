#pragma once

#include <ember/containers/pool.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/render/common.h>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <type_traits>

namespace ember::render
{
	/**
	 * GPU mirrors of scene state, mirrored again in shaders/render/scene.slang.
	 * StructuredBuffer elements stride at std430 rules, so every struct here keeps
	 * its size at a multiple of 16 bytes.
	 */

	/// One object as culling sees it. The sphere is world space so the cull kernel
	/// reads nothing else; it is rewritten whenever the transform changes.
	struct ObjectData
	{
		glm::vec4 sphere = {}; // xyz center, w radius
		u32 geometry	 = 0;  // slot in  the geometry table
		u32 material	 = 0;  // slot in the material table
		u32 flags		 = 0;  // ObjectFlags
		u32 layers		 = 0;  // LayerMask; zero marks a dead slot
	};

	/// Rows of the world matrix's upper 3x4. Row major so shaders reconstruct
	/// a position with three dot products.
	struct TransformData
	{
		glm::vec4 rows[3] = {
			{1.0f, 0.0f, 0.0f, 0.0f},
			{0.0f, 1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 0.0f},
		};
	};

	static_assert(sizeof(ObjectData) == 32 && std::is_trivially_copyable_v<ObjectData>);
	static_assert(sizeof(TransformData) == 48 && std::is_trivially_copyable_v<TransformData>);

	/// CPU-only companion to ObjectData: what the setters need to rebuild the
	/// world sphere and what sync needs for the transform table.
	struct ObjectCold
	{
		TransformData transform = {};
		glm::vec4 local_sphere	= {}; // authoring space center and radius
	};

	static_assert(sizeof(ObjectCold) == 64);

	[[nodiscard]] inline TransformData pack_transform(const glm::mat4& world) noexcept
	{
		// glm stores column major; rows[i] gathers row i across the four columns.
		return {
			.rows = {
				{world[0][0], world[1][0], world[2][0], world[3][0]},
				{world[0][1], world[1][1], world[2][1], world[3][1]},
				{world[0][2], world[1][2], world[2][2], world[3][2]},
			}};
	}

	/// Sphere through an affine transform. The radius scales by the longest basis axis,
	/// which stays conservative under non-uniform scale.
	[[nodiscard]] inline glm::vec4 transform_sphere(const TransformData& transform, glm::vec4 sphere) noexcept
	{
		const glm::vec4 center = {sphere.x, sphere.y, sphere.z, 1.0f};

		const glm::vec4& x = transform.rows[0];
		const glm::vec4& y = transform.rows[1];
		const glm::vec4& z = transform.rows[2];

		// Column j of the upper 3x3 is the image of basis vector j; its squared
		// length is the squared scale along that axis. One sqrt at the end.
		const f32 scale_x = x.x * x.x + y.x * y.x + z.x * z.x;
		const f32 scale_y = x.y * x.y + y.y * y.y + z.y * z.y;
		const f32 scale_z = x.z * x.z + y.z * y.z + z.z * z.z;

		return {
			glm::dot(x, center),
			glm::dot(y, center),
			glm::dot(z, center),
			sphere.w * std::sqrt(std::max({scale_x, scale_y, scale_z})),
		};
	}

	struct RenderObjectDef
	{
		GeometryHandle geometry = {};
		MaterialHandle material = {};
		glm::mat4 transform		= glm::mat4(1.0);
		glm::vec4 sphere		= {0.0f, 0.0f, 0.0f, 1.0f}; // local space center and radius
		LayerMask layers		= LAYER_DEFAULT;
		ObjectFlags flags		= ObjectFlags::CastsShadow;
	};

	/**
	 * Renderer-owned proxy storage: the game mirrors whatever it considers renderable
	 * into objects here and the renderer never sees game entities.
	 *
	 * Storage is one generational Pool. Hot values are the exact GPU object records, cold
	 * values are the transform and local bounds, so GPU sync is a straight copy of pool
	 * storage. A handle's index is the object's slot in every GPU table.
	 *
	 * Mutations set one dirty bit per slot and append the slot once to a dense list.
	 * GpuScene::sync() drains the list with dirty_slots()/object()/transform() and calls
	 * clear_dirty(). One stream covers both tables: transform changes rewrite the world
	 * sphere in the object record anyway, so split streams would save only the rare
	 * material-only edit.
	 *
	 * destroy_object() scrubs the record to all-zero dead state before the slot dies,
	 * and the scrub rides the dirty list. Slot storage outlives the handle, so sync uploads
	 * the scrub from the dead slot; layer mask zero is what tells the cull kernel to skip it.
	 * Slot reuse inside one frame is safe because uploads travel in the frame's command stream,
	 * ordered before any GPU read of the tables.
	 *
	 * The scene is not thread-safe. Setters assert on stale handles in debug and ignore
	 * them in release; a set on a destroyed proxy is a game lifetime bug.
	 */
	class RenderScene
	{
	public:
		RenderScene() noexcept;
		~RenderScene() noexcept;

		RenderScene(const RenderScene&)			   = delete;
		RenderScene& operator=(const RenderScene&) = delete;

		/// Sizes the pool and dirty tracking once. Capacity is fixed because slot indices
		/// are baked into GPU tables and handles.
		void init(u32 object_capacity) noexcept;

		/// Null handle when the scene is full; the failure is logged.
		[[nodiscard]] RenderObjectHandle create_object(const RenderObjectDef& def) noexcept;

		/// Safe on null and stale handles.
		void destroy_object(RenderObjectHandle handle) noexcept;

		void set_transform(RenderObjectHandle handle, const glm::mat4& world) noexcept;
		void set_material(RenderObjectHandle handle, MaterialHandle material) noexcept;

		[[nodiscard]] bool is_valid(RenderObjectHandle handle) const noexcept;

		/// Live objects.
		[[nodiscard]] u32 object_count() const noexcept;

		/**
		 * High water slot bound: every slot below it has been an object at some point.
		 * Monotonic on purpose; GPU tables and cull dispatches must keep covering scrubbed
		 * slots, so the bound never shrinks on destroy.
		 */
		[[nodiscard]] u32 slot_count() const noexcept;
		[[nodiscard]] u32 capacity() const noexcept;

		// The sync interfce. GpuScene drains these once per frame.
		[[nodiscard]] Span<const u32> dirty_slots() const noexcept;
		void clear_dirty() noexcept;

		/// Slot reads ignore liveness so a destroyed slot serves its scrub record.
		[[nodiscard]] const ObjectData& object(u32 slot) const noexcept;
		[[nodiscard]] const TransformData& transform(u32 slot) const noexcept;

	private:
		void mark_dirty(u32 slot) noexcept;

		Pool<RenderObject, ObjectData, ObjectCold, u32> m_objects;

		// Bits deduplicate, the list preserves touch order for coalescing. Both
		// live in one block so init makes a single allocation.
		u64* m_dirty_bits = nullptr;
		u32* m_dirty_list = nullptr;
		u32 m_dirty_count = 0;

		u32 m_slot_count = 0;

		std::pmr::memory_resource* m_dirty_resource = nullptr;
		void* m_dirty_block							= nullptr;
		size_t m_dirty_block_size					= 0;
	};
}
