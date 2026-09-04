#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/render/features/upscale.h>

namespace ember::render
{
	namespace
	{
		/// Slot 1 block, mirrored in upscale.slang.
		struct UpscaleConstants
		{
			glm::vec2 scene_size;
			glm::vec2 scale; // output pixels per scene texel, per axis
			glm::vec2 subtexel_offset;
			glm::vec2 pad;
		};
		static_assert(sizeof(UpscaleConstants) == 32);

		struct UpscalePush
		{
			u32 scene_color;
			u32 scene_sampler;
		};
	}

	UpscaleFeature::UpscaleFeature(gpu::Device& device, const Def& def) noexcept
		: m_resolution(def.resolution), m_subtexel_offset(def.subtexel_offset)
	{
		m_pipeline = device.create_graphics_pipeline({
			.name		   = "upscale",
			.vertex		   = {.code = def.shader, .entry = "vs_main"},
			.fragment	   = {.code = def.shader, .entry = "fs_main"},
			.color_formats = {def.output_format},
		});

		// Linear filtering does the edge band; the shader sharpens the UVs.
		m_sampler = device.create_sampler({
			.name	   = "upscale.linear_clamp",
			.address_u = gpu::AddressMode::ClampToEdge,
			.address_v = gpu::AddressMode::ClampToEdge,
		});

		if (m_pipeline.is_null() || m_sampler.is_null()) [[unlikely]]
			EMBER_ERROR("upscale feature creation failed");
	}

	void UpscaleFeature::shutdown(gpu::Device& device) noexcept
	{
		if (!m_pipeline.is_null())
			device.destroy(m_pipeline);
		if (!m_sampler.is_null())
			device.destroy(m_sampler);

		m_pipeline = {};
		m_sampler  = {};
	}

	void UpscaleFeature::prepare(RenderFrame& frame) noexcept
	{
		if (m_pipeline.is_null()) [[unlikely]]
			return;

		frame.resources.scene_extent = m_resolution;

		frame.resources.scene_color = frame.graph.create({
			.name	= "scene_color",
			.format = gpu::TextureFormat::RGBA16Float,
			.usage	= gpu::TextureUsage::Sampled | gpu::TextureUsage::ColorTarget,
			.extent = m_resolution,
		});
	}

	void UpscaleFeature::add_passes(RenderFrame& frame) noexcept
	{
		if (m_pipeline.is_null() || frame.resources.scene_color.is_null()) [[unlikely]]
			return;

		const Extent2D output = frame.resources.output_extent;

		const UpscaleConstants constants{
			.scene_size = {static_cast<f32>(m_resolution.width), static_cast<f32>(m_resolution.height)},
			.scale =
				{std::max(1.0f, static_cast<f32>(output.width) / static_cast<f32>(m_resolution.width)),
				 std::max(1.0f, static_cast<f32>(output.height) / static_cast<f32>(m_resolution.height))},
			.subtexel_offset = m_subtexel_offset != nullptr ? *m_subtexel_offset : glm::vec2{},
		};

		frame.graph.pass("upscale")
			.read(frame.resources.scene_color)
			.color({.texture = frame.resources.output, .load = gpu::LoadOp::DontCare})
			.record(
				[constants, pipeline = m_pipeline, sampler = m_sampler, scene_color = frame.resources.scene_color](
					gpu::CommandList& cmd, const PassContext& ctx)
				{
					cmd.set_pipeline(pipeline);
					cmd.set_constants(1, constants);
					cmd.set_push_constants(
						UpscalePush{
							.scene_color   = ctx.bindless(scene_color),
							.scene_sampler = bindless_index(sampler),
						});
					cmd.draw(3);
				});
	}
}
