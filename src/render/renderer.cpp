#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/render/renderer.h>

namespace ember::render
{
	Renderer::Renderer() noexcept : m_features(&memory::heap(MemoryTag::Graphics)) {}

	void Renderer::init(gpu::Device& device, const RendererDef& def) noexcept
	{
		EMBER_ASSERT(m_device == nullptr && "init runs once");
		EMBER_ASSERT(is_valid(def));

		m_device = &device;
		m_features.reserve(16);

		m_scene.init(def.object_capacity);
		m_geometry.init(device, def.geometry);
		m_gpu_scene.init(device, {.object_capacity = def.object_capacity});
		m_visibility.init(device, {.cull_shader = def.cull_shader, .command_capacity = def.command_capacity});
		m_readback.init(device);
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

		m_readback.shutdown(device);
		m_visibility.shutdown(device);
		m_gpu_scene.shutdown(device);
		m_geometry.shutdown(device);
		m_graph.shutdown(device);

		m_device = nullptr;
	}

	void Renderer::render(const View& main_view, const RenderOutput& output, u32 frame_slot) noexcept
	{
		EMBER_ASSERT(m_device != nullptr && "render before init");

		m_gpu_scene.sync(*m_device, m_scene);

		m_graph.begin();

		RenderFrame frame{
			.device		= *m_device,
			.scene		= m_scene,
			.gpu_scene	= m_gpu_scene,
			.geometry	= m_geometry,
			.graph		= m_graph,
			.frame_slot = frame_slot,
		};

		frame.resources.output		  = m_graph.import(output.texture, output.initial, output.final_state, output.extent);
		frame.resources.output_extent = output.extent;

		(void)frame.add_view(main_view);

		for (const FeatureEntry& entry : m_features)
			entry.feature->build_views(frame);

		frame.views_locked = true;

		for (u32 view = 0; view < frame.view_count; ++view)
			frame.visibility[view] = m_visibility.cull(m_graph, m_gpu_scene, m_geometry, frame.views[view]);

		for (const FeatureEntry& entry : m_features)
			entry.feature->add_passes(frame);

		// After the consumers: the copies land behind the draws and each count
		// walks write, indirect read, copy source without a round trip.
		for (u32 view = 0; view < frame.view_count; ++view)
			m_readback.capture(m_graph, frame.visibility[view].opaque.count, frame_slot, view);

		m_graph.execute(*m_device);
	}
}
