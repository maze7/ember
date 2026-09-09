#include <ember/core/logger.h>
#include <ember/render/scene.h>

#include <algorithm>
#include <cstring>

namespace ember::render
{
	RenderScene::RenderScene() noexcept : m_objects(MemoryTag::Graphics), m_dirty(MemoryTag::Graphics) {}

	RenderScene::~RenderScene() noexcept = default;

	void RenderScene::init(u32 object_capacity) noexcept
	{
		EMBER_ASSERT(object_capacity != 0 && object_capacity <= decltype(m_objects)::MAX_CAPACITY);

		m_objects.init(object_capacity);
		m_dirty.init(object_capacity);
	}

	RenderObjectHandle RenderScene::create_object(const RenderObjectDef& def) noexcept
	{
		const TransformData transform = pack_transform(def.transform);

		const RenderObjectHandle handle = m_objects.insert(
			ObjectData{
				.sphere	  = transform_sphere(transform, def.sphere),
				.geometry = def.geometry.index,
				.material = def.material.index,
				.flags	  = static_cast<u32>(def.flags),
				.layers	  = def.layers,
			},
			ObjectCold{
				.transform	  = transform,
				.local_sphere = def.sphere,
			});

		if (handle.is_null()) [[unlikely]]
		{
			EMBER_ERROR("render scene is full ({} objects)", m_objects.capacity());
			return handle;
		}

		m_slot_count = std::max(m_slot_count, handle.index + 1);
		m_dirty.mark(handle.index);

		return handle;
	}

	void RenderScene::destroy_object(RenderObjectHandle handle) noexcept
	{
		ObjectData* record = m_objects.get(handle);
		if (record == nullptr)
			return;

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

		record->material = material.index;
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
}
