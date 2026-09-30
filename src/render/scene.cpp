#include <ember/core/logger.h>
#include <ember/math/color.h>
#include <ember/render/material_registry.h>
#include <ember/render/scene.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>

namespace ember::render
{
	namespace
	{
		/// What a scene without a registry accepts: any index a u16 handle can carry.
		constexpr u32 ANY_MATERIAL = 65536;

		/// Copies up to a row of per-object data and zeroes the rest, so a short struct never leaves
		/// the last owner's bytes behind.
		void write_instance(InstanceData& instance, Span<const u8> data) noexcept
		{
			const size_t size = std::min(data.size(), sizeof(instance.bytes));

			if (size != 0)
				std::memcpy(instance.bytes, data.data(), size);

			std::memset(instance.bytes + size, 0, sizeof(instance.bytes) - size);
		}
	}

	LightData pack_light(const LightDef& def) noexcept
	{
		// A zero direction goes nowhere; straight down is the harmless reading of it.
		const f32 length = glm::length(def.direction);

		LightData light{
			.direction = length > 0.0f ? def.direction / length : glm::vec3{0.0f, -1.0f, 0.0f},
			.color	   = linear_from_srgb(def.color) * def.intensity,
			.flags	   = def.casts_shadow ? LIGHT_CASTS_SHADOW : 0u,
		};

		if (def.type == LightType::Directional)
			return light;

		light.position = def.position;
		light.range	   = std::max(def.range, 1e-3f); // zero would read as a directional light
		light.falloff  = 1.0f / std::max(def.source_radius * def.source_radius, 1e-6f);

		if (def.type == LightType::Spot)
		{
			// The cosine falls as the angle opens, so the remap takes the outer angle's to 0 and the
			// inner angle's to 1. The floor keeps a hard-edged cone, inner equal to outer, finite.
			const f32 cos_outer = std::cos(def.outer_angle);
			const f32 cos_inner = std::max(std::cos(def.inner_angle), cos_outer + 1e-4f);

			light.cone_scale  = 1.0f / (cos_inner - cos_outer);
			light.cone_offset = -cos_outer * light.cone_scale;
		}

		return light;
	}

	RenderScene::RenderScene() noexcept
		: m_objects(MemoryTag::Graphics), m_dirty(MemoryTag::Graphics),
		  m_material_users(&memory::heap(MemoryTag::Graphics)), m_instance_written(&memory::heap(MemoryTag::Graphics)),
		  m_lights(MemoryTag::Graphics)
	{
	}

	RenderScene::~RenderScene() noexcept = default;

	void RenderScene::init(u32 object_capacity, const MaterialRegistry* materials, u32 light_capacity) noexcept
	{
		EMBER_ASSERT(object_capacity != 0 && object_capacity <= decltype(m_objects)::MAX_CAPACITY);
		EMBER_ASSERT(light_capacity != 0 && light_capacity <= decltype(m_lights)::MAX_CAPACITY);

		m_objects.init(object_capacity);
		m_dirty.init(object_capacity);
		m_lights.init(light_capacity);

		m_materials = materials;
		m_material_users.assign(materials != nullptr ? materials->material_capacity() : ANY_MATERIAL, 0);
		m_instance_written.assign(object_capacity, 0);
	}

	RenderObjectHandle RenderScene::create_object(const RenderObjectDef& def) noexcept
	{
		EMBER_ASSERT(def.instance.size() <= material::INSTANCE_BYTES && "per-object data is at most a row");

		const TransformData transform = pack_transform(def.transform);
		const u32 material			  = material_index(def.material);

		ObjectCold cold{
			.transform	  = transform,
			.local_sphere = def.sphere,
		};

		// An object starts as its material's type says, unless the caller says otherwise.
		const bool seeded = def.instance.empty() && m_materials != nullptr;
		write_instance(cold.instance, seeded ? m_materials->instance_defaults(def.material) : def.instance);

		const RenderObjectHandle handle = m_objects.insert(
			ObjectData{
				.sphere	  = transform_sphere(transform, def.sphere),
				.geometry = def.geometry.index,
				.material = material,
				.flags	  = static_cast<u32>(def.flags),
				.layers	  = def.layers,
			},
			cold);

		if (handle.is_null()) [[unlikely]]
		{
			EMBER_ERROR("render scene is full ({} objects)", m_objects.capacity());
			return handle;
		}

		++m_material_users[material];
		m_instance_written[handle.index] = def.instance.empty() ? 0 : 1;
		m_slot_count					 = std::max(m_slot_count, handle.index + 1);
		m_dirty.mark(handle.index);

		return handle;
	}

	void RenderScene::destroy_object(RenderObjectHandle handle) noexcept
	{
		ObjectData* record = m_objects.get(handle);
		if (record == nullptr)
			return;

		--m_material_users[record->material];

		// Scrub while the handle is still live; the bytes outlive the erase and
		// sync uploads them, which is what retires the slot on the GPU (layers
		// zero, so culling skips it).
		*record = {};
		m_dirty.mark(handle.index);

		const bool erased = m_objects.erase(handle);
		EMBER_ASSERT(erased);
		(void)erased;
	}

	void RenderScene::set_transform(RenderObjectHandle handle, const glm::mat4& world) noexcept
	{
		ObjectData* record = m_objects.get(handle);

		EMBER_ASSERT(record != nullptr && "set_transform on a dead handle");
		if (record == nullptr) [[unlikely]]
			return;

		// get() validated the handle, so cold storage is addressed directly
		// instead of paying a second generation compare.
		ObjectCold& cold = m_objects.cold_data()[handle.index];

		cold.transform = pack_transform(world);
		record->sphere = transform_sphere(cold.transform, cold.local_sphere);

		m_dirty.mark(handle.index);
	}

	void RenderScene::set_material(RenderObjectHandle handle, MaterialHandle material) noexcept
	{
		ObjectData* record = m_objects.get(handle);

		EMBER_ASSERT(record != nullptr && "set_material on a dead handle");
		if (record == nullptr) [[unlikely]]
			return;

		const u32 next	   = material_index(material);
		const u32 previous = std::exchange(record->material, next);

		// Other threads may be moving objects onto or off the same materials; the counts are only
		// read after they are joined, so relaxed is enough.
		if (previous != next)
		{
			std::atomic_ref(m_material_users[previous]).fetch_sub(1, std::memory_order_relaxed);
			std::atomic_ref(m_material_users[next]).fetch_add(1, std::memory_order_relaxed);
		}

		// Data laid out for one type means nothing to another: given a material of another type, the
		// object starts over from that type's defaults, as a new object would.
		if (m_materials != nullptr && m_materials->bucket_of(previous) != m_materials->bucket_of(next))
		{
			write_instance(m_objects.cold_data()[handle.index].instance, m_materials->instance_defaults(material));
			m_instance_written[handle.index] = 0;
		}

		m_dirty.mark(handle.index);
	}

	void RenderScene::set_instance(RenderObjectHandle handle, Span<const u8> data) noexcept
	{
		EMBER_ASSERT(data.size() <= material::INSTANCE_BYTES && "per-object data is at most a row");

		ObjectData* record = m_objects.get(handle);

		EMBER_ASSERT(record != nullptr && "set_instance on a dead handle");
		if (record == nullptr) [[unlikely]]
			return;

		write_instance(m_objects.cold_data()[handle.index].instance, data);
		m_instance_written[handle.index] = 1;
		m_dirty.mark(handle.index);
	}

	bool RenderScene::is_valid(RenderObjectHandle handle) const noexcept { return m_objects.contains(handle); }

	u32 RenderScene::object_count() const noexcept { return m_objects.size(); }

	u32 RenderScene::slot_count() const noexcept { return m_slot_count; }

	u32 RenderScene::capacity() const noexcept { return m_objects.capacity(); }

	Span<const u32> RenderScene::dirty_slots() const noexcept { return m_dirty.slots(); }

	void RenderScene::clear_dirty() noexcept { m_dirty.clear(); }

	const ObjectData& RenderScene::object(u32 slot) const noexcept
	{
		EMBER_ASSERT(slot < m_slot_count);
		return m_objects.hot_data()[slot];
	}

	const TransformData& RenderScene::transform(u32 slot) const noexcept
	{
		EMBER_ASSERT(slot < m_slot_count);
		return m_objects.cold_data()[slot].transform;
	}

	const InstanceData& RenderScene::instance(u32 slot) const noexcept
	{
		EMBER_ASSERT(slot < m_slot_count);
		return m_objects.cold_data()[slot].instance;
	}

	Span<const u32> RenderScene::material_users() const noexcept
	{
		return {m_material_users.data(), m_material_users.size()};
	}

	LightHandle RenderScene::create_light(const LightDef& def) noexcept
	{
		const LightHandle handle = m_lights.insert(pack_light(def));

		if (handle.is_null()) [[unlikely]]
			EMBER_ERROR("render scene is full ({} lights)", m_lights.capacity());

		return handle;
	}

	void RenderScene::destroy_light(LightHandle handle) noexcept { (void)m_lights.erase(handle); }

	void RenderScene::set_light(LightHandle handle, const LightDef& def) noexcept
	{
		LightData* light = m_lights.get(handle);

		EMBER_ASSERT(light != nullptr && "set_light on a dead handle");
		if (light == nullptr) [[unlikely]]
			return;

		*light = pack_light(def);
	}

	bool RenderScene::is_valid(LightHandle handle) const noexcept { return m_lights.contains(handle); }

	u32 RenderScene::light_count() const noexcept { return m_lights.size(); }

	u32 RenderScene::copy_lights(Span<LightData> out) const noexcept
	{
		u32 count = 0;

		for (const LightData& light : m_lights)
		{
			if (count == out.size())
				break;

			out[count++] = light;
		}

		return count;
	}

	u32 RenderScene::material_index(MaterialHandle material) const noexcept
	{
		// Before init there are no counts, and create_object refuses the object anyway.
		EMBER_ASSERT((m_material_users.empty() || material.index < m_material_users.size()) &&
					 "a material index past the key table");
		return material.index < m_material_users.size() ? material.index : 0;
	}

	void RenderScene::reseed(Span<const MaterialHandle> materials) noexcept
	{
		if (materials.empty() || m_materials == nullptr)
			return;

		// The live ones, sorted by index: objects store their material's index, so one search each
		// finds whether theirs is listed. A dead handle's index may be another material's by now.
		Vector<MaterialHandle> listed(&memory::heap(MemoryTag::Graphics));

		for (const MaterialHandle material : materials)
			if (!m_materials->type_of(material).is_null())
				listed.push_back(material);

		const auto by_index = [](MaterialHandle a, MaterialHandle b) { return a.index < b.index; };
		std::sort(listed.begin(), listed.end(), by_index);

		// One pass over every object, which is fine for what calls this: a type rebuilt, a material
		// moved, a few times in a session.
		for (auto it = m_objects.begin(); it != m_objects.end(); ++it)
		{
			const u32 slot = it.handle().index;

			if (m_instance_written[slot] != 0)
				continue;

			const auto found =
				std::lower_bound(listed.begin(), listed.end(), it->material,
								 [](MaterialHandle material, u32 index) { return material.index < index; });

			if (found == listed.end() || found->index != it->material)
				continue;

			write_instance(m_objects.cold_data()[slot].instance, m_materials->instance_defaults(*found));
			m_dirty.mark(slot);
		}
	}
}
