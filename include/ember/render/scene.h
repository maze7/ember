#pragma once

#include <ember/containers/dirty_set.h>
#include <ember/containers/pool.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/material/type.h>
#include <ember/render/common.h>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <type_traits>

namespace ember::render
{
	class MaterialRegistry;

	/**
	 * GPU mirrors of scene state, mirrored again in shaders/render.slang.
	 * StructuredBuffer elements stride at std430 rules, so every struct here keeps
	 * its size at a multiple of 16 bytes.
	 */

	/// One object as culling sees it. The sphere is world space so the cull kernel
	/// reads nothing else; it is rewritten whenever the transform changes.
	struct ObjectData
	{
		glm::vec4 sphere = {}; // xyz center, w radius
		u32 geometry	 = 0;  // slot in  the geometry table
		u32 material	 = 0;  // the material handle's index: its row of the key table
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

	/**
	 * An object's per-object material data: the bytes its type's `struct Instance` reads, fields
	 * packed in declaration order with no padding, colours linear. What instance<T>() in a material
	 * loads, from the instance table's row for the object's slot.
	 */
	struct InstanceData
	{
		u8 bytes[material::INSTANCE_BYTES] = {};
	};

	static_assert(sizeof(ObjectData) == 32 && std::is_trivially_copyable_v<ObjectData>);
	static_assert(sizeof(TransformData) == 48 && std::is_trivially_copyable_v<TransformData>);
	static_assert(sizeof(InstanceData) == 32 && std::is_trivially_copyable_v<InstanceData>);

	/// CPU-only companion to ObjectData: what the setters need to rebuild the world sphere, and
	/// what sync uploads to the transform and instance tables.
	struct ObjectCold
	{
		TransformData transform = {};
		glm::vec4 local_sphere	= {}; // authoring space center and radius
		InstanceData instance	= {};
	};

	static_assert(sizeof(ObjectCold) == 96);

	[[nodiscard]] inline TransformData pack_transform(const glm::mat4& world) noexcept
	{
		// glm stores column major; rows[i] gathers row i across the four columns.
		return {.rows = {
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

		/// Per-object data to start with, at most material::INSTANCE_BYTES. Empty takes the
		/// material's Instance [Default]s.
		Span<const u8> instance = {};
	};

	/**
	 * Renderer-owned proxy storage: the game mirrors whatever it considers renderable
	 * into objects here and the renderer never sees game entities.
	 *
	 * Storage is one generational Pool. Hot values are the exact GPU object records, cold
	 * values are the transform, local bounds and per-object data, so GPU sync is a straight
	 * copy of pool storage. A handle's index is the object's slot in every GPU table.
	 *
	 * Mutations set one dirty bit per slot and append the slot once to a dense list.
	 * GpuScene::sync() drains the list with dirty_slots()/object()/transform()/instance() and
	 * calls clear_dirty(). One stream covers every table: transform changes rewrite the world
	 * sphere in the object record anyway, and a slot's three rows are 112 bytes, so split
	 * streams would save little until per-frame instance writes dominate a profile.
	 *
	 * The scene counts the live objects naming each material index. The renderer turns those
	 * counts into each draw bucket's share of an argument buffer, which is how the cull can
	 * sort any number of objects into any number of buckets with one pass and no overflow.
	 *
	 * An object's per-object data starts as its material type's Instance defaults and follows
	 * them until the game writes it: when the material moves to another type, or its type is
	 * rebuilt with other defaults, reseed() hands such objects the new ones. Data the game wrote
	 * stays the game's, until the game gives the object a material of another type.
	 *
	 *
	 * destroy_object() scrubs the record to all-zero dead state before the slot dies,
	 * and the scrub rides the dirty list. Slot storage outlives the handle, so sync uploads
	 * the scrub from the dead slot; layer mask zero is what tells the cull kernel to skip it.
	 * Slot reuse inside one frame is safe because uploads travel in the frame's command stream,
	 * ordered before any GPU read of the tables.
	 *
	 * Setters assert on stale handles in debug and ignore them in release; a set on a destroyed
	 * proxy is a game lifetime bug.
	 *
	 * Threading: set_transform, set_material and set_instance run on any frame thread at once,
	 * provided no two calls name the same object; each one writes its own slot and marks a dirty
	 * bit, and set_material moves its counts atomically. Creating and destroying move bookkeeping
	 * every reader depends on, so they stay in the owner phase, and dirty_slots()/clear_dirty()
	 * and material_users() belong to the sync phase after the writers are joined.
	 */
	class RenderScene
	{
	public:
		RenderScene() noexcept;
		~RenderScene() noexcept;

		RenderScene(const RenderScene&)			   = delete;
		RenderScene& operator=(const RenderScene&) = delete;

		/**
		 * Sizes the pool and dirty tracking once. Capacity is fixed because slot indices are baked
		 * into GPU tables and handles. With a registry, new objects start with their material's
		 * Instance defaults, and material indices stay within its key table; without one, which
		 * only tests want, per-object data starts zeroed and any u16 index is accepted.
		 */
		void init(u32 object_capacity, const MaterialRegistry* materials = nullptr) noexcept;

		/// Null handle when the scene is full; the failure is logged.
		[[nodiscard]] RenderObjectHandle create_object(const RenderObjectDef& def) noexcept;

		/// Safe on null and stale handles.
		void destroy_object(RenderObjectHandle handle) noexcept;

		/// Parallel safe on distinct handles.
		void set_transform(RenderObjectHandle handle, const glm::mat4& world) noexcept;

		/**
		 * Keeps the per-object data while the new material draws the same type. A material of another
		 * type starts it over from that type's defaults, since data laid out for one type means
		 * nothing to another. Parallel safe on distinct handles.
		 */
		void set_material(RenderObjectHandle handle, MaterialHandle material) noexcept;

		/**
		 * Replaces the object's per-object data: the bytes of its type's `struct Instance`, packed in
		 * declaration order, colours linear, at most material::INSTANCE_BYTES; the rest is zeroed.
		 * This is what animation and hit flashes write every frame instead of creating materials.
		 * From here the data is the game's, and a type change underneath the material leaves it be.
		 * Parallel safe on distinct handles.
		 */
		void set_instance(RenderObjectHandle handle, Span<const u8> data) noexcept;

		/**
		 * Objects of these materials whose per-object data the game has not written take their
		 * material's Instance defaults again: what the renderer does each frame with the registry's
		 * reseeds(). Stale handles are skipped. Sync phase.
		 */
		void reseed(Span<const MaterialHandle> materials) noexcept;

		/// The typed form: T mirrors the Instance struct field for field.
		template <class T> void set_instance(RenderObjectHandle handle, const T& data) noexcept
		{
			static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= material::INSTANCE_BYTES,
						  "per-object data is at most material::INSTANCE_BYTES of plain bytes");
			set_instance(handle, {reinterpret_cast<const u8*>(&data), sizeof(T)});
		}

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
		[[nodiscard]] const InstanceData& instance(u32 slot) const noexcept;

		/// Live objects naming each material index, one count per index the scene accepts.
		[[nodiscard]] Span<const u32> material_users() const noexcept;

	private:
		/// The index an object stores: a material's, or the error material's for one past the key
		/// table, which could only come from a fabricated handle.
		[[nodiscard]] u32 material_index(MaterialHandle material) const noexcept;

		Pool<RenderObject, ObjectData, ObjectCold, u32> m_objects;
		DirtySet m_dirty;
		Vector<u32> m_material_users; // indexed by material index

		/// Per slot, whether the game wrote the object's per-object data, which then no longer follows
		/// the defaults. A byte each, so two objects never share one and set_instance runs in parallel.
		Vector<u8> m_instance_written;

		const MaterialRegistry* m_materials = nullptr;
		u32 m_slot_count					= 0;
	};
}
