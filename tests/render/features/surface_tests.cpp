#include <ember/gpu/device.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/arena.h>
#include <ember/render/features/surface.h>
#include <ember/render/renderer.h>
#include <ember/shader/compiler.h>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::render;

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
	 * A renderer with the surface feature, drawing into a small target from an orthographic camera
	 * looking down -z at a row of unit quads. Each frame runs the way the app runs one: begin, render,
	 * end, and here a wait, so the counts can be read. Validation is on, and a test fails on anything
	 * the layers saw.
	 */
	class Drawing : public testing::Test
	{
	protected:
		/// The probe compiles once, through the engine's compiler, like any plain program.
		static void SetUpTestSuite()
		{
			shader::Compiler compiler;
			ASSERT_TRUE(compiler.initialize());

			shader::Program program;
			String diagnostics;
			ASSERT_TRUE(compiler.compile_program("pixel_probe.slang", PROBE_SOURCE, program, diagnostics))
				<< diagnostics;

			s_probe = new std::vector<u32>(program.spirv.begin(), program.spirv.end());
		}

		static void TearDownTestSuite()
		{
			delete s_probe;
			s_probe = nullptr;
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
								.geometry		  = {.max_geometries = 4, .vertex_capacity = 64, .index_capacity = 64},
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

			m_renderer.add_feature<SurfaceFeature>({
				.color_format = gpu::TextureFormat::RGBA8Unorm,
				.clear		  = {0.0f, 0.0f, 1.0f, 1.0f},
			});
			m_renderer.add_feature<CountProbe>({.readback = m_readback});
			m_renderer.add_feature<PixelProbe>({.pipeline = m_probe, .readback = m_pixels, .extent = TARGET});

			m_scratch.init(memory::tagged_heap(), "surface.test");
			m_quad = make_quad();
		}

		void TearDown() override
		{
			if (!m_device)
				return;

			for (RenderObjectHandle object : m_objects)
				m_renderer.scene().destroy_object(object);

			for (MaterialHandle material : m_materials)
				m_renderer.materials().destroy(material);

			m_renderer.geometry().destroy(m_device, m_quad);
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

		GeometryHandle make_quad()
		{
			const glm::vec4 positions[4] = {
				{-0.5f, -0.5f, 0.0f, 1.0f},
				{0.5f, -0.5f, 0.0f, 1.0f},
				{-0.5f, 0.5f, 0.0f, 1.0f},
				{0.5f, 0.5f, 0.0f, 1.0f},
			};

			const AttributeData attributes[4] = {
				{.uv = {0.0f, 1.0f}},
				{.uv = {1.0f, 1.0f}},
				{.uv = {0.0f, 0.0f}},
				{.uv = {1.0f, 0.0f}},
			};

			const u32 indices[6] = {0, 1, 2, 2, 1, 3};

			return m_renderer.geometry().create(
				m_device, {
							  .name		  = "test.quad",
							  .positions  = {reinterpret_cast<const u8*>(positions), sizeof(positions)},
							  .attributes = {reinterpret_cast<const u8*>(attributes), sizeof(attributes)},
							  .indices	  = {indices, 6},
							  .sphere	  = {0.0f, 0.0f, 0.0f, 0.7072f},
						  });
		}

		MaterialHandle material(StockType type)
		{
			m_materials.push_back(m_renderer.materials().create(m_renderer.materials().stock_type(type)));
			return m_materials.back();
		}

		/// A quad centred at (x, y) on the z = 0 plane; the camera sees [-4, 4] in both.
		RenderObjectHandle object(MaterialHandle material, f32 x, f32 y = 0.0f)
		{
			m_objects.push_back(m_renderer.scene().create_object({
				.geometry  = m_quad,
				.material  = material,
				.transform = glm::translate(glm::mat4(1.0f), {x, y, 0.0f}),
				.sphere	   = {0.0f, 0.0f, 0.0f, 0.7072f},
			}));
			return m_objects.back();
		}

		/// Renders one frame, waits for it, and returns what the cull counted into each bucket.
		std::vector<u32> frame()
		{
			const View view = make_view(glm::lookAt(glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{0.0f}, {0.0f, 1.0f, 0.0f}),
										ortho_reverse_z(4.0f, 4.0f, 0.1f, 10.0f), TARGET);

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

		/// The last frame's pixel at a world position on the z = 0 plane: the camera shows [-4, 4] in
		/// x and y at 8 pixels a unit, y up.
		[[nodiscard]] u32 pixel(f32 x, f32 y)
		{
			const u32 column = static_cast<u32>((x + 4.0f) * 8.0f);
			const u32 row	 = static_cast<u32>((4.0f - y) * 8.0f);
			return static_cast<const u32*>(m_device.mapped(m_pixels))[row * TARGET.width + column];
		}

		static inline std::vector<u32>* s_probe = nullptr;

		gpu::Device m_device{gpu::DeviceDef{.enable_validation = true, .adapter = gpu::AdapterPreference::Any}};
		Renderer m_renderer;
		Arena m_scratch;

		BufferHandle m_readback		  = {};
		BufferHandle m_pixels		  = {};
		TextureHandle m_target		  = {};
		ComputePipelineHandle m_probe = {};
		GeometryHandle m_quad		  = {};

		std::vector<RenderObjectHandle> m_objects;
		std::vector<MaterialHandle> m_materials;
		u64 m_frames = 0;
		u32 m_errors = 0;
	};
}

TEST_F(Drawing, EveryVisibleObjectLandsInItsTypesBucket)
{
	const MaterialHandle unlit	= material(StockType::Unlit);
	const MaterialHandle sprite = material(StockType::Sprite);

	(void)object(unlit, -3.0f);
	(void)object(unlit, -2.0f);
	(void)object(unlit, 40.0f); // outside the view: counted toward the bucket's range, culled from it
	(void)object(sprite, 0.0f);
	(void)object(sprite, 1.0f);
	(void)object(sprite, 2.0f);
	(void)object({}, 3.0f); // no material: the error type draws it

	const std::vector<u32> counts = frame();

	EXPECT_EQ(counts[m_renderer.materials().error_type().index], 1u);
	EXPECT_EQ(counts[bucket(StockType::Unlit)], 2u);
	EXPECT_EQ(counts[bucket(StockType::Sprite)], 3u);
}

TEST_F(Drawing, ObjectsFollowTheirMaterialAcrossFrames)
{
	const MaterialHandle unlit	= material(StockType::Unlit);
	const MaterialHandle sprite = material(StockType::Sprite);

	const RenderObjectHandle moved = object(unlit, -1.0f);
	(void)object(unlit, 0.0f);
	(void)object(sprite, 1.0f);

	EXPECT_EQ(frame()[bucket(StockType::Unlit)], 2u);

	// A material swap moves the object to the other bucket at the next frame.
	m_renderer.scene().set_material(moved, sprite);
	std::vector<u32> counts = frame();
	EXPECT_EQ(counts[bucket(StockType::Unlit)], 1u);
	EXPECT_EQ(counts[bucket(StockType::Sprite)], 2u);

	// A destroyed material's objects draw as the error type, with no object touched.
	m_renderer.materials().destroy(unlit);
	counts = frame();
	EXPECT_EQ(counts[bucket(StockType::Unlit)], 0u);
	EXPECT_EQ(counts[m_renderer.materials().error_type().index], 1u);
}

TEST_F(Drawing, NewObjectsStartWithTheirTypesInstanceDefaults)
{
	const RenderObjectHandle card = object(material(StockType::Sprite), 0.0f);

	f32 frame_rect[4] = {};
	std::memcpy(frame_rect, m_renderer.scene().instance(card.index).bytes, sizeof(frame_rect));

	EXPECT_EQ(frame_rect[2], 1.0f);
	EXPECT_EQ(frame_rect[3], 1.0f);
	EXPECT_EQ(frame()[bucket(StockType::Sprite)], 1u);
}

TEST_F(Drawing, AReplacedTypeDrawsWithItsNewBuild)
{
	const MaterialHandle unlit = material(StockType::Unlit);
	(void)object(unlit, 0.0f);

	EXPECT_EQ(frame()[bucket(StockType::Unlit)], 1u);

	// What hot reload hands over: a new build of the same type, here with other pipeline state.
	// The surface feature rebuilds that type's pipeline before the next frame draws it.
	const MaterialTypeHandle type = m_renderer.materials().stock_type(StockType::Unlit);
	material::Type rebuilt		  = *m_renderer.materials().type(type);
	rebuilt.state.cull			  = gpu::CullMode::None;
	rebuilt.hash				  = material::hash_type(rebuilt);

	ASSERT_TRUE(m_renderer.materials().replace_type(type, std::move(rebuilt)));
	EXPECT_EQ(frame()[bucket(StockType::Unlit)], 1u);
}

TEST_F(Drawing, EachMaterialDrawsWhatItsTypeSays)
{
	MaterialRegistry& materials = m_renderer.materials();

	// Unlit: white texture, red tint, no light at all.
	const MaterialHandle red = material(StockType::Unlit);
	ASSERT_TRUE(materials.set(red, "tint", glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));

	// A mid grey as a colour picker gives it, sRGB 0.5, which the record holds as linear 0.214.
	const MaterialHandle grey = material(StockType::Unlit);
	ASSERT_TRUE(materials.set(grey, "tint", glm::vec4(0.5f, 0.5f, 0.5f, 1.0f)));

	// Sprite: green tint under the default white ambient, the second with its hit flash on.
	const MaterialHandle green = material(StockType::Sprite);
	ASSERT_TRUE(materials.set(green, "tint", glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)));

	(void)object(red, -3.0f);
	(void)object(grey, -3.0f, 2.0f);
	(void)object(green, -1.0f);
	const RenderObjectHandle flashing = object(green, 1.0f);
	(void)object({}, 3.0f);

	struct SpriteInstance
	{
		glm::vec4 frame = {0.0f, 0.0f, 1.0f, 1.0f};
		f32 flash		= 0.0f;
	};

	m_renderer.scene().set_instance(flashing, SpriteInstance{.flash = 1.0f});
	(void)frame();

	EXPECT_EQ(pixel(-3.0f, 0.0f), rgba(255, 0, 0));
	EXPECT_EQ(pixel(-3.0f, 2.0f), rgba(55, 55, 55)); // linear 0.214 in a unorm target
	EXPECT_EQ(pixel(-1.0f, 0.0f), rgba(0, 255, 0));
	EXPECT_EQ(pixel(1.0f, 0.0f), rgba(255, 255, 255)); // the per-object flash, read from the instance table
	EXPECT_EQ(pixel(0.0f, 3.0f), rgba(0, 0, 255));	   // nothing drawn there: the clear

	// The error type's checker: magenta, or magenta at a fifth, depending on the 8-pixel cell.
	const u32 error = pixel(3.0f, 0.0f);
	EXPECT_TRUE(error == rgba(255, 0, 255) || error == rgba(51, 0, 51)) << std::hex << error;
}
