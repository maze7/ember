#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/render/features/mesh.h>

namespace ember::render
{
	namespace
	{
		struct MeshConstants
		{
			glm::mat4 view_proj;
		};
		static_assert(sizeof(MeshConstants) == 64);

		struct MeshPush
		{
			u32 positions;
			u32 attributes;
			u32 objects;
			u32 transforms;
			u32 materials;
		};
		static_assert(sizeof(MeshPush) == 20);
	}

	MeshFeature::MeshFeature(gpu::Device& device, const Def& def) noexcept
		: m_depth_format(def.depth_format), m_clear(def.clear), m_camera_override(def.camera_override)
	{
		m_pipeline = device.create_graphics_pipeline({
			.name		   = "mesh",
			.vertex		   = {.code = def.shader, .entry = "vs_main"},
			.fragment	   = {.code = def.shader, .entry = "fs_main"},
			.color_formats = {def.color_format},
			.depth_format  = def.depth_format,
			.depth_test	   = true,
			.depth_write   = true,
			.cull		   = gpu::CullMode::Back,
		});

		if (m_pipeline.is_null()) [[unlikely]]
		{
			EMBER_ERROR("mesh feature pipeline creation failed");
			return;
		}

		const MeshMaterial error{};

		m_materials.init(
			device,
			{
				.name		  = "mesh.materials",
				.stride		  = sizeof(MeshMaterial),
				.capacity	  = def.material_capacity,
				.error_record = {reinterpret_cast<const u8*>(&error), sizeof(MeshMaterial)},
			});
	}

	void MeshFeature::shutdown(gpu::Device& device) noexcept
	{
		m_materials.shutdown(device);

		if (!m_pipeline.is_null())
			device.destroy(m_pipeline);
		m_pipeline = {};
	}

	void MeshFeature::add_passes(RenderFrame& frame) noexcept
	{
		if (m_pipeline.is_null()) [[unlikely]]
			return;

		m_materials.sync(frame.device);

		const GraphTexture target =
			frame.resources.scene_color.is_null() ? frame.resources.output : frame.resources.scene_color;

		frame.resources.scene_depth = frame.graph.create({
			.name	= "scene_depth",
			.format = m_depth_format,
			.usage	= gpu::TextureUsage::DepthStencilTarget,
			.extent = frame.resources.scene_extent,
		});

		auto& pass = frame.graph.pass("mesh")
						 .color({.texture = target, .clear = m_clear})
						 .depth({.texture = frame.resources.scene_depth, .store = gpu::StoreOp::DontCare});

		read(pass, frame.visibility[0].opaque);

		const View& camera = m_camera_override != nullptr ? *m_camera_override : frame.views[0];
		const MeshConstants constants{.view_proj = camera.view_projection};

		pass.record(
			[constants,
			 pipeline	  = m_pipeline,
			 stream		  = frame.visibility[0].opaque,
			 index_buffer = frame.geometry.index_buffer(),
			 push =
				 MeshPush{
					 .positions	 = frame.geometry.positions_index(),
					 .attributes = frame.geometry.attributes_index(),
					 .objects	 = frame.gpu_scene.objects_index(),
					 .transforms = frame.gpu_scene.transforms_index(),
					 .materials	 = m_materials.table_index(),
				 }](gpu::CommandList& cmd, const PassContext& ctx)
			{
				cmd.set_pipeline(pipeline);
				cmd.set_constants(0, constants);
				cmd.set_index_buffer(index_buffer);
				cmd.set_push_constants(push);
				draw_indexed_stream(cmd, ctx, stream);
			});
	}
}
