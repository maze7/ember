#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/memory/pmr/arena.h>
#include <ember/render/embedded_shaders.h>
#include <ember/render/renderer.h>

#include <cmath>
#include <cstring>

namespace ember::render
{
	namespace
	{
		/// FrameConstants.time wraps here: an hour keeps an f32 of seconds precise to a quarter of a
		/// millisecond, and a wrap an hour apart is a hitch nobody is watching for.
		constexpr f64 TIME_WRAP = 3600.0;
	}

	Renderer::Renderer() noexcept : m_features(&memory::heap(MemoryTag::Graphics)) {}

	void Renderer::init(gpu::Device& device, const RendererDef& def) noexcept
	{
		EMBER_ASSERT(m_device == nullptr && "init runs once");
		EMBER_ASSERT(is_valid(def));

		m_device = &device;
		m_features.reserve(16);

		// The registry first: the scene seeds new objects from its Instance defaults.
		m_materials.init(device, def.materials);
		m_scene.init(def.object_capacity, &m_materials);
		m_geometry.init(device, def.geometry);
		m_gpu_scene.init(device, {.object_capacity = def.object_capacity});
		m_visibility.init(device,
						  {
							  .cull_shader		= def.cull_shader.empty() ? embedded::cull_shader() : def.cull_shader,
							  .command_capacity = def.command_capacity,
							  .bucket_capacity	= m_materials.bucket_count(),
						  });
	}

	void Renderer::shutdown(gpu::Device& device) noexcept
	{
		for (u32 i = static_cast<u32>(m_features.size()); i > 0; --i)
		{
			FeatureEntry& entry = m_features[i - 1];
			entry.feature->shutdown(device);
			entry.destroy(entry.feature);
		}
		m_features.clear();

		m_visibility.shutdown(device);
		m_materials.shutdown(device);
		m_gpu_scene.shutdown(device);
		m_geometry.shutdown(device);
		m_graph.shutdown(device);

		m_device = nullptr;
	}

	void Renderer::render(const View& main_view, const RenderOutput& output, const FrameTiming& timing,
						  Arena& scratch) noexcept
	{
		EMBER_ASSERT(m_device != nullptr && "render before init");

		// Per-object data the game never wrote follows its type's defaults, which a material moving to
		// another type, or a type rebuilt, has just changed. Before the scene uploads what changed.
		m_scene.reseed(m_materials.reseeds());
		m_materials.clear_reseeds();

		m_gpu_scene.sync(*m_device, m_scene, scratch);
		m_materials.sync(*m_device, scratch);

		m_graph.begin(scratch);

		RenderFrame frame{
			.device		= *m_device,
			.scene		= m_scene,
			.gpu_scene	= m_gpu_scene,
			.geometry	= m_geometry,
			.materials	= m_materials,
			.graph		= m_graph,
			.scratch	= scratch,
			.frame_slot = timing.slot,
			.constants	= frame_constants(timing.delta_time),
			.buckets	= layout_buckets(scratch),
		};

		frame.resources.output = m_graph.import(output.texture, output.initial, output.final_state, output.extent);
		frame.resources.output_extent = output.extent;
		frame.resources.scene_extent  = output.extent;

		(void)frame.add_view(main_view);

		for (const FeatureEntry& entry : m_features)
			entry.feature->prepare(frame);

		for (const FeatureEntry& entry : m_features)
			entry.feature->build_views(frame);

		frame.views_locked = true;

		for (u32 view = 0; view < frame.view_count; ++view)
		{
			// Lighting is the frame's, published in prepare(); the matrices are the view's.
			ViewConstants& constants = frame.view_constants[view];
			constants				 = view_constants(frame.views[view]);
			constants.ambient		 = frame.resources.ambient;
			constants.light_count	 = frame.resources.light_count;
			constants.lights		 = frame.resources.lights;

			const View& culled = (view == 0 && m_cull_override != nullptr) ? *m_cull_override : frame.views[view];
			frame.visibility[view] =
				m_visibility.cull(m_graph, frame.constants, frame.buckets, m_gpu_scene.slot_count(), culled);
		}

		for (const FeatureEntry& entry : m_features)
			entry.feature->add_passes(frame);

		m_graph.execute(*m_device);
	}

	FrameConstants Renderer::frame_constants(f32 delta_time) noexcept
	{
		m_time = std::fmod(m_time + delta_time, TIME_WRAP);

		return {
			.time		   = static_cast<f32>(m_time),
			.delta_time	   = delta_time,
			.frame_index   = m_frame_index++,
			.objects	   = m_gpu_scene.objects_index(),
			.transforms	   = m_gpu_scene.transforms_index(),
			.instances	   = m_gpu_scene.instances_index(),
			.material_keys = m_materials.key_table_index(),
			.positions	   = m_geometry.positions_index(),
			.attributes	   = m_geometry.attributes_index(),
			.geometries	   = m_geometry.table_index(),
		};
	}

	DrawBuckets Renderer::layout_buckets(Arena& scratch) noexcept
	{
		const u32 count = m_materials.bucket_count();

		auto* ranges =
			static_cast<BucketRange*>(scratch.allocate_fast(count * sizeof(BucketRange), alignof(BucketRange)));
		const u32 draws = m_materials.layout_buckets(m_scene.material_users(), {ranges, count});

		// The cull reads the ranges from this frame's slice of the transient ring: written once here,
		// published by the submit, gone when the frame retires. The passes read the scratch copy,
		// since the ring is write-combined memory the CPU should never read back.
		const gpu::TransientArray<BucketRange> table = m_device->transient().allocate_array<BucketRange>(count);

		if (!table.valid()) [[unlikely]]
		{
			EMBER_ERROR("transient ring exhausted; no draws this frame");
			return {};
		}

		std::memcpy(table.data, ranges, count * sizeof(BucketRange));

		return {
			.ranges = {ranges, count},
			.table	= bindless_index(table.buffer),
			.first	= table.first_element(),
			.draws	= draws,
		};
	}
}
