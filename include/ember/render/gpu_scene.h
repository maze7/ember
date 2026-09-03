#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/buffer.h>
#include <ember/gpu/common.h>
#include <ember/render/scene.h>

namespace ember::gpu
{
	class Device;
}

namespace ember::render
{
	struct GpuSceneDef
	{
		/// One table slot per scene slot; must cover the RenderScene it mirrors.
		u32 object_capacity = 1u << 16;
	};

	[[nodiscard]] constexpr bool is_valid(const GpuSceneDef& def) noexcept { return def.object_capacity != 0; }

	/**
	 * Walks maximal runs of consecutive values in an ascending slot list and
	 * calls fn(first_slot, count) per run. Sync turns each run into one staged
	 * copy per table, so scattered edits cost few copies and a creation burst
	 * costs exactly one.
	 */
	template <class Fn> void for_each_slot_run(Span<const u32> sorted_slots, Fn&& fn) noexcept
	{
		const u32 count = static_cast<u32>(sorted_slots.size());

		u32 i = 0;
		while (i < count)
		{
			const u32 begin = i;

			while (i + 1 < count && sorted_slots[i + 1] == sorted_slots[i] + 1)
				++i;
			++i;

			fn(sorted_slots[begin], i - begin);
		}
	}

	/**
	 * Persistent GPU tables mirroring RenderScene: one ObjectData and one
	 * TransformData slot per scene slot, addressed by the index the object's
	 * handle carries. Culling and shading read only these tables, which is what
	 * makes the scene GPU driven.
	 *
	 * sync() drains the scene's dirty list, sorts it, and stages one copy per
	 * consecutive slot run and table. Uploads ride the staging ring inside the
	 * frame's command stream: the batch entry barrier orders every prior GPU
	 * read before the copies and the exit barrier publishes the bytes to this
	 * frame's work. That ordering is the entire synchronization story. Scrubs,
	 * creates and same frame slot reuse land exactly once, in submission order,
	 * so the mirror needs no slot quarantine and no per frame versioning.
	 *
	 * The tables are written only by sync(). The day an in frame GPU pass
	 * writes them (scatter uploads, GPU driven LOD), that writer imports the
	 * buffers into the graph and pays the barrier cost explicitly.
	 */
	class GpuScene
	{
	public:
		struct SyncStats
		{
			u32 dirty_slots	  = 0;
			u32 slot_runs	  = 0;
			u32 copy_commands = 0; // two staged copies per run, one per table
			u64 bytes		  = 0;
		};

		GpuScene() = default;

		GpuScene(const GpuScene&)			 = delete;
		GpuScene& operator=(const GpuScene&) = delete;

		/// Creates the tables. Runs once, before the first sync.
		void init(gpu::Device& device, const GpuSceneDef& def) noexcept;

		/// Destroys the tables. Call before the device goes down.
		void shutdown(gpu::Device& device) noexcept;

		/// Uploads every dirty slot and clears the scene's dirty list. Call once
		/// per frame, inside begin/end_frame, before the graph executes.
		void sync(gpu::Device& device, RenderScene& scene) noexcept;

		/// Bindless slots for shaders; slot i of each table is scene slot i.
		[[nodiscard]] u32 objects_index() const noexcept { return bindless_index(m_objects); }
		[[nodiscard]] u32 transforms_index() const noexcept { return bindless_index(m_transforms); }

		/**
		 * High water of the mirror as of the last sync: GPU passes iterate
		 * exactly this many table slots. Zero before the first sync, so a cull
		 * issued early draws nothing instead of reading slots never uploaded.
		 */
		[[nodiscard]] u32 slot_count() const noexcept { return m_slot_count; }

		[[nodiscard]] SyncStats last_sync() const noexcept { return m_last_sync; }

	private:
		BufferHandle m_objects	  = {};
		BufferHandle m_transforms = {};

		u32 m_capacity		  = 0;
		u32 m_slot_count	  = 0;
		SyncStats m_last_sync = {};
	};
}
