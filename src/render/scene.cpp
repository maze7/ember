#include <ember/core/logger.h>
#include <ember/render/scene.h>

#include <algorithm>
#include <cstring>

namespace ember::render
{
	RenderScene::RenderScene() noexcept : m_objects(MemoryTag::Graphics) {}

	RenderScene::~RenderScene() noexcept
	{
		if (m_dirty_block != nullptr)
			m_dirty_resource->deallocate(m_dirty_block, m_dirty_block_size, alignof(u64));
	}

	void RenderScene::init(u32 object_capacity) noexcept
	{
		EMBER_ASSERT(m_dirty_block == nullptr && "init runs once");
		EMBER_ASSERT(object_capacity != 0 && object_capacity <= decltype(m_objects)::MAX_CAPACITY);

		m_objects.init(object_capacity);

		const size_t bits_bytes = ((object_capacity + 63) / 64) * sizeof(u64);

		m_dirty_resource   = &memory::heap(MemoryTag::Graphics);
		m_dirty_block_size = bits_bytes + object_capacity * sizeof(u32);
		m_dirty_block	   = m_dirty_resource->allocate(m_dirty_block_size, alignof(u64));

		m_dirty_bits = static_cast<u64*>(m_dirty_block);
		m_dirty_list = reinterpret_cast<u32*>(static_cast<std::byte*>(m_dirty_block) + bits_bytes);

		std::memset(m_dirty_bits, 0, bits_bytes);
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
		mark_dirty(handle.index);

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
		mark_dirty(handle.index);

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

		mark_dirty(handle.index);
	}

	void RenderScene::set_material(RenderObjectHandle handle, MaterialHandle material) noexcept
	{
		ObjectData* record = m_objects.get(handle);

		EMBER_ASSERT(record != nullptr && "set_material on a dead handle");
		if (record == nullptr) [[unlikely]]
			return;

		record->material = material.index;
		mark_dirty(handle.index);
	}

	bool RenderScene::is_valid(RenderObjectHandle handle) const noexcept { return m_objects.contains(handle); }

	u32 RenderScene::object_count() const noexcept { return m_objects.size(); }

	u32 RenderScene::slot_count() const noexcept { return m_slot_count; }

	u32 RenderScene::capacity() const noexcept { return m_objects.capacity(); }

	Span<const u32> RenderScene::dirty_slots() const noexcept { return {m_dirty_list, m_dirty_count}; }

	void RenderScene::clear_dirty() noexcept
	{
		// Sparse reset: steady state frames dirty a handful of slots and a full
		// memset would touch the whole bitset for them.
		for (u32 i = 0; i < m_dirty_count; ++i)
			m_dirty_bits[m_dirty_list[i] >> 6] &= ~(u64{1} << (m_dirty_list[i] & 63));

		m_dirty_count = 0;
	}

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

	void RenderScene::mark_dirty(u32 slot) noexcept
	{
		u64& word	  = m_dirty_bits[slot >> 6];
		const u64 bit = u64{1} << (slot & 63);

		if ((word & bit) != 0)
			return;

		word						  |= bit;
		m_dirty_list[m_dirty_count++]  = slot;
	}
}
