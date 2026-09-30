#include <ember/core/filesystem.h>
#include <ember/gpu/device.h>
#include <ember/memory/memory.h>
#include <ember/shader/compiler.h>

#include <gtest/gtest.h>

namespace
{
	using namespace ember;

	/**
	 * What the compiler builds, a real device must accept: every stage combination a feature will
	 * ask for, with validation on so the layers check the SPIR-V against the bindless layout and the
	 * features the device enabled. Skips where there is no Vulkan at all.
	 */
	class Pipelines : public testing::Test
	{
	protected:
		void SetUp() override
		{
			if (!m_device)
				GTEST_SKIP() << "no Vulkan adapter";

			ASSERT_TRUE(m_compiler.initialize());
			m_errors = gpu::Device::validation_error_count();
		}

		[[nodiscard]] String engine_file(StringView name) const
		{
			String path;
			EXPECT_TRUE(fs::join(path, m_compiler.engine_dir(), name));
			return path;
		}

		[[nodiscard]] static String read(StringView path)
		{
			auto file = fs::read_file(path, memory::heap(MemoryTag::Tools));
			EXPECT_TRUE(file) << path;
			return file ? String(file->text()) : String();
		}

		material::Type compile_material(StringView path, StringView source)
		{
			material::Type type;
			String diagnostics;
			EXPECT_TRUE(m_compiler.compile_material(path, source, type, diagnostics)) << diagnostics;
			return type;
		}

		shader::Program compile_program(StringView name)
		{
			const String path = engine_file(name);

			shader::Program program;
			String diagnostics;
			EXPECT_TRUE(m_compiler.compile_program(path, read(path), program, diagnostics)) << diagnostics;
			return program;
		}

		void expect_graphics(const char* name, Span<const u8> code, const char* fragment, bool color, bool depth)
		{
			gpu::GraphicsPipelineDef def{
				.name	  = name,
				.vertex	  = {.code = code, .entry = material::VERTEX_ENTRY},
				.fragment = {.code = code, .entry = fragment},
			};

			def.color_count		 = color ? 1 : 0;
			def.color_formats[0] = gpu::TextureFormat::RGBA16Float;
			def.depth_format	 = depth ? gpu::TextureFormat::D32Float : gpu::TextureFormat::Undefined;
			def.depth_test		 = depth;
			def.depth_write		 = depth;

			const GraphicsPipelineHandle pipeline = m_device.create_graphics_pipeline(def);
			EXPECT_FALSE(pipeline.is_null()) << name << " " << fragment;
			m_device.destroy(pipeline);
		}

		gpu::Device m_device{gpu::DeviceDef{.enable_validation = true, .adapter = gpu::AdapterPreference::Any}};
		shader::Compiler m_compiler;
		u32 m_errors = 0;
	};
}

TEST_F(Pipelines, EveryMaterialDomainAndPassCreatesAValidPipeline)
{
	const String error_path	   = engine_file("materials/error.slang");
	const material::Type error = compile_material(error_path, read(error_path));

	// Everything the authoring API offers at once: per-object data, a moved vertex, a normal map,
	// texel snapping, and a discard that lowers to demote.
	const material::Type sprite = compile_material("pipeline_sprite.slang", R"(
import material;

[Queue("cutout")] [Cull("none")]
struct Sprite : IMaterial
{
	[Filter("point")] Texture2DRef albedo;
	[Linear] [Default("flat")] Texture2DRef normals;
	[Color] [Default("1 1 1 1")] float4 tint;
	[Range(0, 1)] [Default("0.5")] float cutoff;
	[Range(0, 2)] float sway;

	struct Instance
	{
		float4 frame;
		float flash;
	};

	override void vertex(inout VertexData v)
	{
		v.position.x += sin(v.time * 2.0 + v.origin.x) * sway * v.uv.y;
	}

	void surface(SurfaceInput s, inout Surface out)
	{
		let data  = instance<Instance>(s);
		let uv    = lerp(data.frame.xy, data.frame.zw, s.uv);
		let texel = albedo.sample(uv) * tint;
		if (texel.a < cutoff)
			discard;

		out.albedo = lerp(texel.rgb, 1.0, data.flash);
		out.normal = s.to_world(unpack_normal(normals.sample(uv)));
		snap_to_texels(s, albedo, uv, out);
	}
};
)");

	const material::Type screen = compile_material("pipeline_screen.slang", R"(
import material;

struct Palette : IScreenMaterial
{
	[Color] float4 tint;
	Texture2DRef ramp;

	float4 pixel(ScreenInput s) { return ramp.sample(float2(luminance(scene_color(s.uv).rgb), 0.5)) * tint; }
};
)");

	for (const material::Type* type : {&error, &sprite})
	{
		expect_graphics(type->name.c_str(), type->bytecode(), material::COLOR_ENTRY, true, true);
		expect_graphics(type->name.c_str(), type->bytecode(), material::DEPTH_ENTRY, false, true);
	}

	expect_graphics(screen.name.c_str(), screen.bytecode(), material::COLOR_ENTRY, true, false);

	m_device.wait_idle();
	EXPECT_EQ(gpu::Device::validation_error_count() - m_errors, 0u);
}

TEST_F(Pipelines, ThePlainProgramsCompileToValidPipelinesToo)
{
	const shader::Program cull = compile_program("cull.slang");
	const ComputePipelineHandle compute =
		m_device.create_compute_pipeline({.name = "cull", .shader = {.code = cull.bytecode(), .entry = "cs_main"}});
	EXPECT_FALSE(compute.is_null());
	m_device.destroy(compute);

	for (const char* name : {"upscale.slang", "imgui.slang"})
		expect_graphics(name, compile_program(name).bytecode(), "fs_main", true, false);

	m_device.wait_idle();
	EXPECT_EQ(gpu::Device::validation_error_count() - m_errors, 0u);
}
