#pragma once

#include <ember/gpu/device.h>
#include <ember/jobs/job_system.h>
#include <ember/math/packing.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/arena.h>
#include <ember/render/features/lighting.h>
#include <ember/render/features/surface.h>
#include <ember/render/renderer.h>
#include <ember/shader/compiler.h>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <utility>
#include <vector>

// The fixture every test that draws shares: a renderer with lighting and the surface feature,
// drawing into a small target whose pixels and bucket counts it reads back.
namespace ember::render::test
{
	/**
	 * Copies the main view's bucket counts where the CPU can read them, after every other pass:
	 * what the cull counted into each bucket, which is what the surface passes drew.
	 */
	class CountProbe final : public RenderFeature
	{
	public:
		struct Def
		{
			BufferHandle readback = {};
		};

		CountProbe(Renderer&, const Def& def) noexcept : m_readback(def.readback) {}

		void add_passes(RenderFrame& frame) noexcept override
		{
			const GraphBuffer counts = frame.visibility[0].counts;
			const u64 bytes			 = frame.buckets.ranges.size() * sizeof(u32);

			frame.graph.pass("probe")
				.read(counts, gpu::BufferState::CopySrc)
				.record([counts, bytes, readback = m_readback](gpu::CommandList& cmd, const PassContext& ctx)
						{ cmd.copy_buffer(ctx.buffer(counts), 0, readback, 0, bytes); });
		}

	private:
		BufferHandle m_readback = {};
	};

	/**
	 * Copies the output's pixels where the CPU can read them. The graph has no texture-to-buffer
	 * copy, so a compute pass loads every texel into a buffer first, which also reads the target the
	 * way any later pass would.
	 */
	class PixelProbe final : public RenderFeature
	{
	public:
		struct Def
		{
			ComputePipelineHandle pipeline = {};
			BufferHandle readback		   = {};
			Extent2D extent				   = {};
		};

		PixelProbe(Renderer&, const Def& def) noexcept : m_def(def) {}

		void add_passes(RenderFrame& frame) noexcept override
		{
			struct Push
			{
				u32 target;
				u32 pixels;
				u32 width;
			};

			const GraphTexture target = frame.resources.output;
			const u64 bytes			  = u64{m_def.extent.width} * m_def.extent.height * sizeof(u32);
			const GraphBuffer pixels  = frame.graph.create({
				.name  = "probe.pixels",
				.size  = bytes,
				.usage = gpu::BufferUsage::Storage | gpu::BufferUsage::CopySrc,
			});

			frame.graph.pass("probe.load")
				.read(target)
				.write(pixels)
				.record(
					[def = m_def, target, pixels](gpu::CommandList& cmd, const PassContext& ctx)
					{
						cmd.set_pipeline(def.pipeline);
						cmd.set_push_constants(Push{ctx.bindless(target), ctx.bindless(pixels), def.extent.width});
						cmd.dispatch(def.extent.width / 8, def.extent.height / 8);
					});

			frame.graph.pass("probe.copy")
				.read(pixels, gpu::BufferState::CopySrc)
				.record([pixels, bytes, readback = m_def.readback](gpu::CommandList& cmd, const PassContext& ctx)
						{ cmd.copy_buffer(ctx.buffer(pixels), 0, readback, 0, bytes); });
		}

	private:
		Def m_def = {};
	};

	/// The probe's kernel: each texel packed as RGBA8, row after row.
	constexpr const char* PROBE_SOURCE = R"(
import ember;

struct Push
{
	uint target;
	uint pixels;
	uint width;
};

[[vk::push_constant]] ConstantBuffer<Push> g_push;
[[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] RWStructuredBuffer<uint> g_pixels[];

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
	const uint4 c = uint4(saturate(texture_2d(g_push.target).Load(int3(id.xy, 0))) * 255.0 + 0.5);
	g_pixels[g_push.pixels][id.y * g_push.width + id.x] = c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);
}
)";

	constexpr Extent2D TARGET = {64, 64};

	/// An RGBA8 pixel as the probe packs it.
	[[nodiscard]] constexpr u32 rgba(u32 r, u32 g, u32 b, u32 a = 255) noexcept
	{
		return r | (g << 8) | (b << 16) | (a << 24);
	}

	/**
	 * A renderer with the lighting and surface features, drawing into a small target from an
	 * orthographic camera looking down -z at a row of unit quads. The lighting starts at its
	 * defaults, a white sky and ground and no lights, so a lit type draws its albedo until a test
	 * says otherwise. Each frame runs the way the app runs one: begin, render, end, and here a wait,
	 * so the counts can be read. Validation is on, and a test fails on anything the layers saw.
	 */
	class Drawing : public ::testing::Test
	{
	protected:
		/// One compiler for the suite: the probe compiles once, like any plain program, and tests
		/// build the material types they need through it.
		static void SetUpTestSuite()
		{
			s_compiler = new shader::Compiler;
			ASSERT_TRUE(s_compiler->initialize());

			shader::Program program;
			String diagnostics;
			ASSERT_TRUE(s_compiler->compile_program("pixel_probe.slang", PROBE_SOURCE, program, diagnostics))
				<< diagnostics;

			s_probe = new std::vector<u32>(program.spirv.begin(), program.spirv.end());
		}

		static void TearDownTestSuite()
		{
			delete s_probe;
			delete s_compiler;
			s_probe	   = nullptr;
			s_compiler = nullptr;
		}

		void SetUp() override
		{
			if (!m_device)
				GTEST_SKIP() << "no Vulkan adapter";

			m_errors = gpu::Device::validation_error_count();
			jobs::initialize({.worker_count = 2});

			m_renderer.init(m_device,
							{
								.object_capacity  = 64,
								.command_capacity = 64,
								.geometry		  = {.max_geometries = 8, .vertex_capacity = 64, .index_capacity = 64},
								.materials		  = {.max_types = 8, .max_materials = 32, .initial_records = 4},
							});

			m_readback = m_device.create_buffer({
				.name	= "test.counts",
				.size	= m_renderer.materials().bucket_count() * sizeof(u32),
				.usage	= gpu::BufferUsage::CopyDst,
				.memory = gpu::MemoryLocation::Readback,
			});

			m_pixels = m_device.create_buffer({
				.name	= "test.pixels",
				.size	= u64{TARGET.width} * TARGET.height * sizeof(u32),
				.usage	= gpu::BufferUsage::CopyDst,
				.memory = gpu::MemoryLocation::Readback,
			});

			m_target = m_device.create_texture({
				.name	= "test.target",
				.extent = {TARGET.width, TARGET.height, 1},
				.format = gpu::TextureFormat::RGBA8Unorm,
				.usage	= gpu::TextureUsage::ColorTarget | gpu::TextureUsage::Sampled,
			});

			m_probe = m_device.create_compute_pipeline({
				.name	= "test.probe",
				.shader = {.code  = {reinterpret_cast<const u8*>(s_probe->data()), s_probe->size() * sizeof(u32)},
						   .entry = "cs_main"},
			});

			add_features();
			m_renderer.add_feature<CountProbe>({.readback = m_readback});
			m_renderer.add_feature<PixelProbe>({.pipeline = m_probe, .readback = m_pixels, .extent = TARGET});

			m_scratch.init(memory::tagged_heap(), "drawing.test");
			m_quad = quad();
		}

		void TearDown() override
		{
			if (!m_device)
				return;

			for (RenderObjectHandle object : m_objects)
				m_renderer.scene().destroy_object(object);

			for (MaterialHandle material : m_materials)
				m_renderer.materials().destroy(material);

			for (GeometryHandle geometry : m_geometries)
				m_renderer.geometry().destroy(m_device, geometry);

			m_renderer.shutdown(m_device);

			m_device.destroy(m_probe);
			m_device.destroy(m_target);
			m_device.destroy(m_pixels);
			m_device.destroy(m_readback);
			m_device.wait_idle();

			jobs::shutdown();
			m_scratch.shutdown();

			EXPECT_EQ(gpu::Device::validation_error_count() - m_errors, 0u);
		}

		/// The features under test, registered ahead of the probes: lighting, and the surfaces drawing
		/// straight into the target. A dither cell the size of the camera's pixel, 8 pixels a unit, and
		/// shadows fit to every height the camera sees. A fixture testing another arrangement of
		/// features overrides this.
		virtual void add_features()
		{
			m_lighting = &m_renderer.add_feature<LightingFeature>({
				.dither_cell = 1.0f / 8.0f,
				.shadow		 = {.resolution = 512, .min_height = -8.0f, .max_height = 8.0f},
			});
			m_renderer.add_feature<SurfaceFeature>({
				.color_format = gpu::TextureFormat::RGBA8Unorm,
				.clear		  = {0.0f, 0.0f, 1.0f, 1.0f},
			});
		}

		/// A unit quad on the z = 0 plane, facing the camera, whose vertices carry this normal. Shading
		/// reads the normal alone, so a test can tilt it without turning the quad edge-on.
		GeometryHandle quad(glm::vec3 normal = {0.0f, 0.0f, 1.0f})
		{
			const glm::vec4 positions[4] = {
				{-0.5f, -0.5f, 0.0f, 1.0f},
				{0.5f, -0.5f, 0.0f, 1.0f},
				{-0.5f, 0.5f, 0.0f, 1.0f},
				{0.5f, 0.5f, 0.0f, 1.0f},
			};

			const u32 packed = pack_octahedral(glm::normalize(normal));

			const AttributeData attributes[4] = {
				{.normal = packed, .uv = {0.0f, 1.0f}},
				{.normal = packed, .uv = {1.0f, 1.0f}},
				{.normal = packed, .uv = {0.0f, 0.0f}},
				{.normal = packed, .uv = {1.0f, 0.0f}},
			};

			const u32 indices[6] = {0, 1, 2, 2, 1, 3};

			m_geometries.push_back(m_renderer.geometry().create(
				m_device, {
							  .name		  = "test.quad",
							  .positions  = {reinterpret_cast<const u8*>(positions), sizeof(positions)},
							  .attributes = {reinterpret_cast<const u8*>(attributes), sizeof(attributes)},
							  .indices	  = {indices, 6},
							  .sphere	  = {0.0f, 0.0f, 0.0f, 0.7072f},
						  }));
			return m_geometries.back();
		}

		/// A type compiled from source, the way the asset system builds one from a file.
		MaterialTypeHandle type(const char* file, const char* source)
		{
			material::Type compiled;
			String diagnostics;

			if (!s_compiler->compile_material(file, source, compiled, diagnostics))
			{
				ADD_FAILURE() << diagnostics;
				return {};
			}

			return m_renderer.materials().add_type(std::move(compiled));
		}

		MaterialHandle material(MaterialTypeHandle type)
		{
			m_materials.push_back(m_renderer.materials().create(type));
			return m_materials.back();
		}

		MaterialHandle material(StockType type) { return material(m_renderer.materials().stock_type(type)); }

		/// A quad centred at a position; the camera looks down -z and sees [-4, 4] in x and y. Without
		/// a geometry, the camera-facing quad.
		RenderObjectHandle object(MaterialHandle material, glm::vec3 position, GeometryHandle geometry = {},
								  ObjectFlags flags = ObjectFlags::CastsShadow, LayerMask layers = LAYER_DEFAULT)
		{
			m_objects.push_back(m_renderer.scene().create_object({
				.geometry  = geometry.is_null() ? m_quad : geometry,
				.material  = material,
				.transform = glm::translate(glm::mat4(1.0f), position),
				.sphere	   = {0.0f, 0.0f, 0.0f, 0.7072f},
				.layers	   = layers,
				.flags	   = flags,
			}));
			return m_objects.back();
		}

		/// A quad centred at (x, y) on the z = 0 plane.
		RenderObjectHandle object(MaterialHandle material, f32 x, f32 y = 0.0f, GeometryHandle geometry = {})
		{
			return object(material, {x, y, 0.0f}, geometry);
		}

		/// Renders one frame, waits for it, and returns what the cull counted into each bucket.
		std::vector<u32> frame()
		{
			const View view = make_view(glm::lookAt(glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{0.0f}, {0.0f, 1.0f, 0.0f}),
										ortho_reverse_z(4.0f, 4.0f, 0.1f, 10.0f), TARGET, m_view_layers);

			m_scratch.begin(heap_tag(MemoryLifetime::RenderScratch, ++m_frames));

			const gpu::FrameInfo info = m_device.begin_frame();
			m_renderer.render(view,
							  {
								  .texture	   = m_target,
								  .extent	   = TARGET,
								  .final_state = gpu::TextureState::RenderTarget,
							  },
							  {.slot = info.slot, .delta_time = 1.0f / 60.0f}, m_scratch);
			(void)m_device.end_frame();
			m_device.wait_idle();

			memory::tagged_heap().free(m_scratch.end());

			const auto* counts = static_cast<const u32*>(m_device.mapped(m_readback));
			return {counts, counts + m_renderer.materials().bucket_count()};
		}

		[[nodiscard]] u16 bucket(StockType type) const { return m_renderer.materials().stock_type(type).index; }

		/// The last frame's pixel at a column and row of the target.
		[[nodiscard]] u32 texel(u32 column, u32 row)
		{
			return static_cast<const u32*>(m_device.mapped(m_pixels))[row * TARGET.width + column];
		}

		/// The last frame's pixel at a world position on the z = 0 plane: the camera shows [-4, 4] in
		/// x and y at 8 pixels a unit, y up.
		[[nodiscard]] u32 pixel(f32 x, f32 y)
		{
			return texel(static_cast<u32>((x + 4.0f) * 8.0f), static_cast<u32>((4.0f - y) * 8.0f));
		}

		static inline shader::Compiler* s_compiler = nullptr;
		static inline std::vector<u32>* s_probe	   = nullptr;

		gpu::Device m_device{gpu::DeviceDef{.enable_validation = true, .adapter = gpu::AdapterPreference::Any}};
		Renderer m_renderer;
		Arena m_scratch;

		BufferHandle m_readback		  = {};
		BufferHandle m_pixels		  = {};
		TextureHandle m_target		  = {};
		ComputePipelineHandle m_probe = {};
		GeometryHandle m_quad		  = {};
		LightingFeature* m_lighting	  = nullptr;

		std::vector<RenderObjectHandle> m_objects;
		std::vector<MaterialHandle> m_materials;
		std::vector<GeometryHandle> m_geometries;
		LayerMask m_view_layers = LAYER_ALL; // what the camera draws
		u64 m_frames			= 0;
		u32 m_errors			= 0;
	};
}
