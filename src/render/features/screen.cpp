#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/render/embedded_shaders.h>
#include <ember/render/features/screen.h>

#include <glm/packing.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <utility>

namespace ember::render
{
	namespace
	{
		/// Every target the feature draws. Half floats keep the light over 1 that bloom and the
		/// shoulder work on, and a chain of screen materials never rounds what the next one reads.
		constexpr gpu::TextureFormat FORMAT = gpu::TextureFormat::RGBA16Float;

		/// What differs between passes, mirrored in shaders/post.slang. The textures are the graph's,
		/// filled in as each pass records.
		struct PostPush
		{
			u32 source		= 0;
			u32 bloom		= 0;
			u32 sampler		= 0;
			f32 threshold	= 0.0f;
			glm::vec2 texel = {};
		};

		static_assert(sizeof(PostPush) <= PUSH_CONSTANT_BYTES);

		/// The grade's knobs, mirrored in shaders/post.slang and bound at CONSTANTS_PASS.
		struct GradeConstants
		{
			f32 bloom	 = 0.0f;
			f32 exposure = 1.0f;
			f32 knee	 = 1.0f;
			f32 blend	 = 0.0f;

			u32 luts[2]		 = {};
			f32 lut_sizes[2] = {};
		};

		static_assert(sizeof(GradeConstants) == 32);

		/// Mirror of screen.slang's DrawConstants: the material's table, and its record's slot in it.
		struct DrawConstants
		{
			u32 materials = 0;
			u32 slot	  = 0;
		};

		[[nodiscard]] glm::vec2 texel_of(Extent2D extent) noexcept
		{
			return {1.0f / static_cast<f32>(extent.width), 1.0f / static_cast<f32>(extent.height)};
		}

		/// A bloom pass's recording: the pipeline, the push block with the source the graph resolved,
		/// and one triangle over the target.
		[[nodiscard]] auto record_filter(GraphicsPipelineHandle pipeline, GraphTexture source, PostPush push) noexcept
		{
			return [pipeline, source, push](gpu::CommandList& cmd, const PassContext& ctx)
			{
				PostPush bound = push;
				bound.source   = ctx.bindless(source);

				cmd.set_pipeline(pipeline);
				cmd.set_push_constants(bound);
				cmd.draw(3);
			};
		}
	}

	ColorLut create_color_lut(gpu::Device& device, u32 size, glm::vec3 (*grade)(glm::vec3)) noexcept
	{
		EMBER_ASSERT(grade != nullptr);
		if (grade == nullptr)
			return {};

		using Plain = glm::vec3 (*)(glm::vec3);
		return create_color_lut(
			device, size, [](glm::vec3 color, const void* plain) { return (*static_cast<const Plain*>(plain))(color); },
			&grade);
	}

	ColorLut create_color_lut(gpu::Device& device, u32 size, glm::vec3 (*grade)(glm::vec3, const void*),
							  const void* context) noexcept
	{
		// 64 cells a side is already a 4096 texel strip, and finer than any grade needs.
		EMBER_ASSERT(size >= 2 && size <= 64 && grade != nullptr);
		if (size < 2 || size > 64 || grade == nullptr)
			return {};

		Vector<u32> texels(size * size * size, 0u, &memory::heap(MemoryTag::Graphics));
		const f32 step = 1.0f / static_cast<f32>(size - 1);

		// Row by row as the strip lies: green down it, then blue slice by slice, red across each.
		for (u32 g = 0; g < size; ++g)
		{
			for (u32 b = 0; b < size; ++b)
			{
				for (u32 r = 0; r < size; ++r)
				{
					const glm::vec3 display{static_cast<f32>(r) * step, static_cast<f32>(g) * step,
											static_cast<f32>(b) * step};

					texels[(g * size + b) * size + r] = glm::packUnorm4x8(glm::vec4(grade(display, context), 1.0f));
				}
			}
		}

		// sRGB, like a strip loaded from a PNG: the grade's display values are stored as they are,
		// and the hardware decodes them to linear on the way back out.
		const TextureHandle texture = device.create_texture({
			.name		  = "color_lut",
			.extent		  = {size * size, size, 1},
			.format		  = gpu::TextureFormat::RGBA8Srgb,
			.initial_data = {reinterpret_cast<const u8*>(texels.data()), texels.size() * sizeof(u32)},
		});

		return {texture, texture.is_null() ? 0 : size};
	}

	ScreenFeature::ScreenFeature(Renderer& renderer, const Def& def) noexcept
		: m_pipelines(renderer.materials().bucket_count(), TypePipeline{}, &memory::heap(MemoryTag::Graphics))
	{
		gpu::Device& gpu			= renderer.gpu();
		const Span<const u8> shader = def.shader.empty() ? embedded::post_shader() : def.shader;

		const auto build = [&](const char* name, const char* entry, gpu::BlendPreset blend)
		{
			return gpu.create_graphics_pipeline({
				.name		   = name,
				.vertex		   = {.code = shader, .entry = "vs_main"},
				.fragment	   = {.code = shader, .entry = entry},
				.color_formats = {FORMAT},
				.blend		   = blend,
			});
		};

		m_down	= build("screen.bloom.down", "fs_down", gpu::BlendPreset::Opaque);
		m_up	= build("screen.bloom.up", "fs_up", gpu::BlendPreset::Additive);
		m_grade = build("screen.grade", "fs_grade", gpu::BlendPreset::Opaque);

		// Linear, so a tap between texels averages them; clamped, so the edges never pull in the far side.
		m_sampler = gpu.create_sampler({
			.name	   = "screen.linear_clamp",
			.address_u = gpu::AddressMode::ClampToEdge,
			.address_v = gpu::AddressMode::ClampToEdge,
		});

		if (m_down.is_null() || m_up.is_null() || m_grade.is_null() || m_sampler.is_null()) [[unlikely]]
			EMBER_ERROR("screen feature creation failed");

		set_bloom(def.bloom);
		set_tone(def.exposure, def.knee);
	}

	void ScreenFeature::shutdown(gpu::Device& device) noexcept
	{
		for (TypePipeline& entry : m_pipelines)
			destroy(device, entry);

		device.destroy(std::exchange(m_down, {}));
		device.destroy(std::exchange(m_up, {}));
		device.destroy(std::exchange(m_grade, {}));
		device.destroy(std::exchange(m_sampler, {}));
	}

	void ScreenFeature::prepare(RenderFrame& frame) noexcept
	{
		// The surface feature's scheme for its buckets, for the screen types: a pipeline outlives its
		// type by at most a frame, a type pays for its pipeline the first frame it exists, and a type
		// hot reload rebuilt gets a new one. A failed build keeps its null pipeline until the type
		// moves again; the device logged why.
		for (TypePipeline& entry : m_pipelines)
		{
			if (entry.type.is_null())
				continue;

			const material::Type* type = frame.materials.type(entry.type);

			if (type == nullptr || type->domain != material::Domain::Screen)
				destroy(frame.device, entry);
		}

		frame.materials.for_each_type(
			[&](MaterialTypeHandle handle, const material::Type& type, u32 generation) noexcept
			{
				if (type.domain != material::Domain::Screen)
					return;

				TypePipeline& entry = m_pipelines[handle.index];

				if (entry.type == handle && entry.generation == generation)
					return;

				destroy(frame.device, entry);

				// Its bytecode and blend, and the target only this feature knows: no depth, no culling.
				const GraphicsPipelineHandle pipeline = frame.device.create_graphics_pipeline({
					.name		   = type.name.c_str(),
					.vertex		   = {.code = type.bytecode(), .entry = material::VERTEX_ENTRY},
					.fragment	   = {.code = type.bytecode(), .entry = material::COLOR_ENTRY},
					.color_formats = {FORMAT},
					.blend		   = type.state.blend,
				});

				entry = {handle, generation, pipeline, type.state.blend != gpu::BlendPreset::Opaque};
			});
	}

	void ScreenFeature::add_passes(RenderFrame& frame) noexcept
	{
		// Without an intermediate scene colour the world drew straight into the output, and there is
		// nothing here to read.
		if (m_grade.is_null() || frame.resources.scene_color.is_null()) [[unlikely]]
			return;

		const BloomChain bloom	  = add_bloom(frame);
		const GraphTexture graded = add_grade(frame, bloom);

		// The upscale reads whatever this leaves here.
		frame.resources.scene_color = add_materials(frame, graded);
	}

	ScreenFeature::BloomChain ScreenFeature::add_bloom(RenderFrame& frame) const noexcept
	{
		if (m_bloom.intensity <= 0.0f)
			return {};

		// Each level halves the one above, rounding up so its last texel still covers the edge.
		GraphTexture levels[MAX_BLOOM_LEVELS];
		Extent2D extents[MAX_BLOOM_LEVELS];
		Extent2D extent = frame.resources.scene_extent;
		u32 count		= 0;

		while (count < m_bloom.levels && extent.width > 1 && extent.height > 1)
		{
			extent			= {(extent.width + 1) / 2, (extent.height + 1) / 2};
			extents[count]	= extent;
			levels[count++] = frame.graph.create({.name = "screen.bloom", .format = FORMAT, .extent = extent});
		}

		if (count == 0)
			return {};

		const u32 sampler = bindless_index(m_sampler);

		// Down: each level filters the one above it, and the first keeps only the light over the
		// threshold.
		GraphTexture source	 = frame.resources.scene_color;
		Extent2D source_size = frame.resources.scene_extent;
		f32 threshold		 = m_bloom.threshold;

		for (u32 i = 0; i < count; ++i)
		{
			const PostPush push{.sampler = sampler, .threshold = threshold, .texel = texel_of(source_size)};

			frame.graph.pass("screen.bloom.down")
				.read(source)
				.color({.texture = levels[i], .load = gpu::LoadOp::DontCare})
				.record(record_filter(m_down, source, push));

			source		= levels[i];
			source_size = extents[i];
			threshold	= 0.0f;
		}

		// Up: each level adds the one below it, which by then holds everything below that, so the top
		// ends up with every level's light.
		for (u32 i = count - 1; i > 0; --i)
		{
			const PostPush push{.sampler = sampler, .texel = texel_of(extents[i])};

			frame.graph.pass("screen.bloom.up")
				.read(levels[i])
				.color({.texture = levels[i - 1], .load = gpu::LoadOp::Load})
				.record(record_filter(m_up, levels[i], push));
		}

		return {levels[0], extents[0], count};
	}

	GraphTexture ScreenFeature::add_grade(RenderFrame& frame, const BloomChain& bloom) const noexcept
	{
		const GraphTexture scene  = frame.resources.scene_color;
		const GraphTexture target = frame.graph.create({
			.name	= "screen.graded",
			.format = FORMAT,
			.extent = frame.resources.scene_extent,
		});

		// Each level is a normalised blur of the same light, so the chain's sum is `levels` times it:
		// dividing here makes intensity the share of that light the glow adds back.
		const GradeConstants constants{
			.bloom	   = bloom.levels > 0 ? m_bloom.intensity / static_cast<f32>(bloom.levels) : 0.0f,
			.exposure  = m_exposure,
			.knee	   = m_knee,
			.blend	   = m_blend,
			.luts	   = {bindless_index(m_from.texture), bindless_index(m_to.texture)},
			.lut_sizes = {static_cast<f32>(m_from.size), static_cast<f32>(m_to.size)},
		};

		const PostPush push{
			.sampler = bindless_index(m_sampler),
			.texel	 = bloom.levels > 0 ? texel_of(bloom.extent) : glm::vec2{},
		};

		RenderGraph::Pass& pass =
			frame.graph.pass("screen.grade").read(scene).color({.texture = target, .load = gpu::LoadOp::DontCare});

		if (!bloom.top.is_null())
			pass.read(bloom.top);

		pass.record(
			[pipeline = m_grade, scene, top = bloom.top, constants, push](gpu::CommandList& cmd, const PassContext& ctx)
			{
				PostPush bound = push;
				bound.source   = ctx.bindless(scene);
				bound.bloom	   = top.is_null() ? 0 : ctx.bindless(top);

				cmd.set_pipeline(pipeline);
				cmd.set_constants(CONSTANTS_PASS, constants);
				cmd.set_push_constants(bound);
				cmd.draw(3);
			});

		return target;
	}

	GraphTexture ScreenFeature::add_materials(RenderFrame& frame, GraphTexture image) const noexcept
	{
		const GraphTexture depth = frame.resources.scene_depth;

		// Nearest: depth must not blend across a silhouette, nor the image between its pixels.
		const u32 sampler =
			bindless_index(frame.materials.sampler(gpu::Filter::Nearest, gpu::AddressMode::ClampToEdge));

		GraphTexture spare = {}; // the target the image is not in, made when a material first needs it

		for (u32 i = 0; i < m_material_count; ++i)
		{
			const MaterialHandle material = m_materials[i];
			const MaterialTypeHandle type = frame.materials.type_of(material);

			if (type.is_null() || type.index >= m_pipelines.size())
				continue;

			const TypePipeline& entry = m_pipelines[type.index];

			if (entry.type != type || entry.pipeline.is_null())
				continue;

			// A material that returns the whole image reads this one and writes the spare, and the two
			// trade places. One that blends draws over this one in place, and cannot read it.
			GraphTexture source = {};
			GraphTexture target = image;

			if (!entry.blends)
			{
				if (spare.is_null())
					spare = frame.graph.create(
						{.name = "screen.image", .format = FORMAT, .extent = frame.resources.scene_extent});

				source = image;
				target = std::exchange(spare, image);
			}

			RenderGraph::Pass& pass =
				frame.graph.pass("screen.material")
					.color({.texture = target, .load = entry.blends ? gpu::LoadOp::Load : gpu::LoadOp::DontCare});

			if (!source.is_null())
				pass.read(source);
			if (!depth.is_null())
				pass.read(depth);

			const DrawConstants draw{frame.materials.table_index(type), frame.materials.slot(material)};

			// The view's constants with the targets filled in, which the graph resolves only as the
			// pass records. A blended material reads colour as the heap's fallback.
			pass.record(
				[pipeline = entry.pipeline, draw, constants = frame.constants, view = frame.view_constants[0], source,
				 depth, sampler](gpu::CommandList& cmd, const PassContext& ctx)
				{
					ViewConstants bound = view;
					bound.scene_color	= source.is_null() ? 0 : ctx.bindless(source);
					bound.scene_depth	= depth.is_null() ? 0 : ctx.bindless(depth);
					bound.scene_sampler = sampler;

					cmd.set_pipeline(pipeline);
					bind_scene_constants(cmd, constants, bound);
					cmd.set_push_constants(draw);
					cmd.draw(3);
				});

			image = target;
		}

		return image;
	}

	void ScreenFeature::set_bloom(const Bloom& bloom) noexcept
	{
		m_bloom = {
			.intensity = std::max(bloom.intensity, 0.0f),
			.threshold = std::max(bloom.threshold, 0.0f),
			.levels	   = std::min(bloom.levels, MAX_BLOOM_LEVELS),
		};
	}

	void ScreenFeature::set_tone(f32 exposure, f32 knee) noexcept
	{
		m_exposure = std::max(exposure, 0.0f);
		m_knee	   = std::clamp(knee, 0.0f, 1.0f);
	}

	void ScreenFeature::set_grade(ColorLut from, ColorLut to, f32 blend) noexcept
	{
		// A table without a cell on each side of a colour is no table: the identity.
		m_from	= from.size >= 2 && !from.texture.is_null() ? from : ColorLut{};
		m_to	= to.size >= 2 && !to.texture.is_null() ? to : ColorLut{};
		m_blend = std::clamp(blend, 0.0f, 1.0f);
	}

	void ScreenFeature::set_materials(Span<const MaterialHandle> materials) noexcept
	{
		EMBER_ASSERT(materials.size() <= MAX_SCREEN_MATERIALS);

		m_material_count = static_cast<u32>(std::min<size_t>(materials.size(), MAX_SCREEN_MATERIALS));
		std::copy_n(materials.data(), m_material_count, m_materials);
	}

	void ScreenFeature::destroy(gpu::Device& device, TypePipeline& entry) noexcept
	{
		device.destroy(std::exchange(entry, {}).pipeline);
	}
}
