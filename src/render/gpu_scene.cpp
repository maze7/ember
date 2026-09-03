#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/memory/pmr/arena_resource.h>
#include <ember/render/gpu_scene.h>

#include <algorithm>
#include <cstring>

namespace ember::render
{
	void GpuScene::init(gpu::Device& device, const GpuSceneDef& def) noexcept
	{
		EMBER_ASSERT(m_objects.is_null() && "init runs once");
		EMBER_ASSERT(is_valid(def));

		m_capacity = def.object_capacity;

		m_objects = device.create_buffer({
			.name  = "gpu_scene.objects",
			.size  = u64{def.object_capacity} * sizeof(ObjectData),
			.usage = gpu::BufferUsage::Storage,
		});

		m_transforms = device.create_buffer({
			.name  = "gpu_scene.transforms",
			.size  = u64{def.object_capacity} * sizeof(TransformData),
			.usage = gpu::BufferUsage::Storage,
		});

		if (m_objects.is_null() || m_transforms.is_null())
		{
			EMBER_ERROR("gpu scene table creation failed");
			shutdown(device);
		}
	}

	void GpuScene::shutdown(gpu::Device& device) noexcept
	{
		if (!m_objects.is_null())
			device.destroy(m_objects);
		if (!m_transforms.is_null())
			device.destroy(m_transforms);

		m_objects	 = {};
		m_transforms = {};
		m_capacity	 = 0;
	}

	void GpuScene::sync(gpu::Device& device, RenderScene& scene) noexcept
	{
		EMBER_ASSERT(!m_objects.is_null() && "sync before init");
		EMBER_ASSERT(scene.capacity() <= m_capacity && "tables must cover every scene slot");

		m_last_sync = {};

		const Span<const u32> dirty = scene.dirty_slots();
		if (dirty.empty())
			return;

		const u32 count = static_cast<u32>(dirty.size());

		// The scene's list is append ordered; sorting turns it into runs. Both
		// scratch blocks die with the frame arena and their bytes are consumed
		// by update_buffer during the call.
		auto* slots = static_cast<u32*>(memory::frame_arena().allocate_fast(count * sizeof(u32), alignof(u32)));
		std::memcpy(slots, dirty.data(), count * sizeof(u32));
		std::sort(slots, slots + count);

		// Transforms sit inside the scene's cold stride, so they gather into a
		// packed copy once, in sorted order. Object records upload straight from
		// pool storage because a slot run is contiguous there.
		auto* transforms = static_cast<TransformData*>(
			memory::frame_arena().allocate_fast(count * sizeof(TransformData), alignof(TransformData)));

		for (u32 i = 0; i < count; ++i)
			transforms[i] = scene.transform(slots[i]);

		u32 cursor = 0;

		for_each_slot_run(
			{slots, count},
			[&](u32 first, u32 run) noexcept
			{
				device.update_buffer(
					m_objects,
					u64{first} * sizeof(ObjectData),
					{reinterpret_cast<const u8*>(&scene.object(first)), run * sizeof(ObjectData)});

				device.update_buffer(
					m_transforms,
					u64{first} * sizeof(TransformData),
					{reinterpret_cast<const u8*>(transforms + cursor), run * sizeof(TransformData)});

				cursor				  += run;
				m_last_sync.copy_runs += 1;
				m_last_sync.bytes	  += run * (sizeof(ObjectData) + sizeof(TransformData));
			});

		m_last_sync.dirty_slots = count;
		scene.clear_dirty();
	}
}
