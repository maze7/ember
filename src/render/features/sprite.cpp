#include "ember/math/packing.h"
#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/render/embedded_shaders.h>
#include <ember/render/features/sprite.h>

namespace ember::render
{
	namespace
	{
		struct SpriteConstants
		{
			glm::mat4 view_proj;
		};
		static_assert(sizeof(SpriteConstants) == 64);

		struct SpritePush
		{
			u32 positions;
			u32 attributes;
			u32 objects;
			u32 transforms;
			u32 materials;
		};
		static_assert(sizeof(SpritePush) == 20);
	}

	SpriteFeature::SpriteFeature(Renderer& renderer, const Def& def) noexcept : m_renderer(&renderer)
	{
		auto& gpu	 = renderer.gpu();
		auto& shader = def.shader.empty() ? embedded::sprite_shader() : def.shader;

		m_pipeline = gpu.create_graphics_pipeline({
			.name		   = "sprite",
			.vertex		   = {.code = shader, .entry = "vs_main"},
			.fragment	   = {.code = shader, .entry = "fs_main"},
			.color_formats = {def.color_format},
			.depth_format  = def.depth_format,
			.depth_test	   = true,
			.depth_write   = true,
			.cull		   = gpu::CullMode::None,	   // standees are seen from either side under the fly cam
			.blend		   = gpu::BlendPreset::Opaque, // alpha test via discard keeps draws order independent
		});

		if (m_pipeline.is_null()) [[unlikely]]
		{
			EMBER_ERROR("sprite feature pipeline creation failed");
			return;
		}

		// The default card: uv v0 at the top, up normal so baked
		// and per-entity cards shade alike under a lit family.
		const u32 normal = pack_octahedral({0.0f, 1.0f, 0.0f});

		const glm::vec4 positions[4] = {
			{0.0f, 1.0f, 0.0f, 1.0f},
			{1.0f, 1.0f, 0.0f, 1.0f},
			{0.0f, 0.0f, 0.0f, 1.0f},
			{1.0f, 0.0f, 0.0f, 1.0f},
		};

		const AttributeData attributes[4] = {
			{normal, 0xFFFFFFFF, {0.0f, 0.0f}},
			{normal, 0xFFFFFFFF, {1.0f, 0.0f}},
			{normal, 0xFFFFFFFF, {0.0f, 1.0f}},
			{normal, 0xFFFFFFFF, {1.0f, 1.0f}},
		};

		const u32 indices[6] = {0, 2, 1, 1, 2, 3};

		m_quad = renderer.geometry().create(
			gpu,
			{
				.name		= "sprite.quad",
				.positions	= {reinterpret_cast<const u8*>(positions), sizeof(positions)},
				.attributes = {reinterpret_cast<const u8*>(attributes), sizeof(attributes)},
				.indices	= {indices, 6},
				.sphere		= SPRITE_QUAD_SPHERE,
			});

		const SpriteMaterial error{};

		m_materials.init(
			gpu,
			{
				.name		  = "sprite.materials",
				.stride		  = sizeof(SpriteMaterial),
				.capacity	  = def.material_capacity,
				.error_record = {reinterpret_cast<const u8*>(&error), sizeof(SpriteMaterial)},
			});
	}

	void SpriteFeature::shutdown(gpu::Device& device) noexcept
	{
		m_materials.shutdown(device);
		m_renderer->geometry().destroy(device, m_quad);
		m_quad = {};

		if (!m_pipeline.is_null())
			device.destroy(m_pipeline);
		m_pipeline = {};
	}

	void SpriteFeature::add_passes(RenderFrame& frame) noexcept
	{
		if (m_pipeline.is_null()) [[unlikely]]
			return;

		m_materials.sync(frame.device);

		// The world family owns the color and depth targets; sprites compose onto
		// them. Without a world family ahead there is nothing to stand on.
		if (frame.resources.scene_color.is_null() || frame.resources.scene_depth.is_null()) [[unlikely]]
			return;

		auto& pass = frame.graph.pass("sprite")
						 .color({.texture = frame.resources.scene_color, .load = gpu::LoadOp::Load})
						 .depth(
							 {.texture = frame.resources.scene_depth,
							  .load	   = gpu::LoadOp::Load,
							  .store   = gpu::StoreOp::DontCare});

		read(pass, frame.visibility[0].cutout);

		const SpriteConstants constants{.view_proj = frame.views[0].view_projection};

		pass.record(
			[constants,
			 pipeline	  = m_pipeline,
			 stream		  = frame.visibility[0].cutout,
			 index_buffer = frame.geometry.index_buffer(),
			 push =
				 SpritePush{
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
