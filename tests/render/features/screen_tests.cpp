#include "drawing.h"

#include <ember/render/features/screen.h>
#include <ember/render/features/upscale.h>

using namespace ember;
using namespace ember::render;
using ember::render::test::Drawing;
using ember::render::test::rgba;
using ember::render::test::TARGET;

namespace
{
	/// One linear colour, unlit and unbounded, so the scene holds exactly the value a test sets, light
	/// over 1 included.
	constexpr const char* GLOW_SOURCE = R"(
import material;

[Shading("unlit")]
struct Glow : IMaterial
{
	[Color] [Hdr] [Default("1 1 1 1")] float4 color;

	void surface(SurfaceInput s, inout Surface out) { out.albedo = color.rgb; }
};
)";

	constexpr const char* INVERT_SOURCE = R"(
import material;

struct Invert : IScreenMaterial
{
	float4 pixel(ScreenInput s)
	{
		const float4 image = scene_color(s.uv);
		return float4(1.0 - image.rgb, image.a);
	}
};
)";

	constexpr const char* SCALE_SOURCE = R"(
import material;

struct Scale : IScreenMaterial
{
	[Default("1")] float factor;

	float4 pixel(ScreenInput s)
	{
		const float4 image = scene_color(s.uv);
		return float4(image.rgb * factor, image.a);
	}
};
)";

	constexpr const char* DEPTH_SOURCE = R"(
import material;

struct Depth : IScreenMaterial
{
	float4 pixel(ScreenInput s) { return float4(scene_depth(s.uv).xxx, 1.0); }
};
)";

	/// A quarter-strength red wash, blended over the image rather than reading it.
	constexpr const char* WASH_SOURCE = R"(
import material;

[Blend("alpha")]
struct Wash : IScreenMaterial
{
	[Color] [Default("1 0 0 0.25")] float4 color;

	float4 pixel(ScreenInput s) { return color; }
};
)";

	/// One channel of a probed pixel: 0 is red, 1 green, 2 blue.
	[[nodiscard]] constexpr u32 channel(u32 pixel, u32 index) noexcept { return (pixel >> (8 * index)) & 0xFFu; }

	/**
	 * The drawing fixture with the screen feature between the surfaces and a 1:1 upscale: the scene
	 * renders in half floats, the screen feature works on them, and the upscale copies the result into
	 * the unorm target texel for texel. The clear is black, so a pixel nothing reached reads zero.
	 */
	class Screen : public Drawing
	{
	protected:
		void add_features() override
		{
			m_renderer.add_feature<SurfaceFeature>({
				.color_format = gpu::TextureFormat::RGBA16Float,
				.clear		  = {0.0f, 0.0f, 0.0f, 1.0f},
			});
			m_screen = &m_renderer.add_feature<ScreenFeature>();
			m_renderer.add_feature<UpscaleFeature>({
				.output_format = gpu::TextureFormat::RGBA8Unorm,
				.resolution	   = TARGET,
			});
		}

		void TearDown() override
		{
			// Every frame waits for the GPU, so nothing still reads the tables.
			if (m_device)
				for (const ColorLut& table : m_tables)
					m_device.destroy(table.texture);

			Drawing::TearDown();
		}

		/// A quad of one linear colour. The material comes back, for a test that changes the colour.
		MaterialHandle glow(glm::vec3 color, glm::vec3 position = {})
		{
			if (m_glow.is_null())
				m_glow = type("glow.slang", GLOW_SOURCE);

			const MaterialHandle flat = material(m_glow);
			EXPECT_TRUE(m_renderer.materials().set(flat, "color", glm::vec4(color, 1.0f)));
			(void)object(flat, position);
			return flat;
		}

		ColorLut table(glm::vec3 (*grade)(glm::vec3))
		{
			m_tables.push_back(create_color_lut(m_device, 32, grade));
			EXPECT_FALSE(m_tables.back().texture.is_null());
			return m_tables.back();
		}

		ScreenFeature* m_screen	  = nullptr;
		MaterialTypeHandle m_glow = {};
		std::vector<ColorLut> m_tables;
	};
}

// A unorm pixel is round(255 * the linear colour), and the scene and every target after it are half
// floats: the values below are picked so neither rounding lands near a tie.

TEST_F(Screen, WithNothingSetTheSceneReachesTheOutputAsItWas)
{
	(void)glow({0.2f, 0.4f, 0.6f}, {-2.0f, 0.0f, 0.0f});
	(void)glow({1.5f, 0.8f, 0.0f}, {2.0f, 0.0f, 0.0f});
	(void)frame();

	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(51, 102, 153));
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(0, 0, 0));

	// Over 1 clips, as the display would clip it anyway.
	EXPECT_EQ(pixel(2.0f, 0.0f), rgba(255, 204, 0));
}

TEST_F(Screen, ExposureScalesAndTheShoulderRollsHighlightsOff)
{
	(void)glow(glm::vec3(0.2f), {-2.0f, 0.0f, 0.0f});
	(void)glow(glm::vec3(0.45f), {0.0f, 0.0f, 0.0f});
	(void)glow(glm::vec3(0.6f), {2.0f, 0.0f, 0.0f});

	m_screen->set_tone(2.0f, 0.8f);
	(void)frame();

	// Doubled, 0.4 is under the knee and passes untouched.
	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(102, 102, 102));

	// 0.9 is 0.1 over it: 0.8 + 0.2 * (1 - e^-0.5) = 0.8787.
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(224, 224, 224));

	// 1.2 would clip to 255; the shoulder keeps it under white: 0.8 + 0.2 * (1 - e^-2) = 0.9729.
	EXPECT_EQ(pixel(2.0f, 0.0f), rgba(248, 248, 248));
}

TEST_F(Screen, TheGradeBlendsBetweenTwoTables)
{
	const ColorLut identity = table([](glm::vec3 c) { return c; });
	const ColorLut invert	= table([](glm::vec3 c) { return 1.0f - c; });

	(void)glow(glm::vec3(1.0f));

	// Black and white sit on the tables' corner cells and read back exact: a quarter of the way from
	// each colour to its inverse.
	m_screen->set_grade(identity, invert, 0.25f);
	(void)frame();

	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(64, 64, 64));
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(191, 191, 191));

	// A missing table is the identity, so a grade can fade in from the ungraded scene.
	m_screen->set_grade({}, invert, 0.25f);
	(void)frame();

	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(64, 64, 64));
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(191, 191, 191));
}

TEST_F(Screen, TablesAreIndexedByDisplayColour)
{
	(void)glow({0.2f, 0.4f, 0.6f});

	// A table that swaps red and blue: a slice picked wrongly, or an axis crossed, shows at once. The
	// colour falls between cells, and cells are 8 bits, so a table is exact to a step either way; only
	// no table at all is exact.
	m_screen->set_grade(table([](glm::vec3 c) { return glm::vec3(c.b, c.g, c.r); }), {}, 0.0f);
	(void)frame();

	const u32 swapped = pixel(0.0f, 0.0f);
	EXPECT_NEAR(channel(swapped, 0), 153, 1);
	EXPECT_NEAR(channel(swapped, 1), 102, 1);
	EXPECT_NEAR(channel(swapped, 2), 51, 1);
}

TEST_F(Screen, BloomSpreadsOnlyTheLightOverTheThreshold)
{
	m_screen->set_bloom({.intensity = 1.0f, .threshold = 1.0f, .levels = 3});

	// The quad covers columns and rows 28 to 35. Under the threshold nothing spreads, and the scene
	// reaches the output exactly.
	const MaterialHandle lamp = glow(glm::vec3(0.8f));
	(void)frame();

	EXPECT_EQ(texel(32, 32), rgba(204, 204, 204));
	EXPECT_EQ(texel(36, 32), rgba(0, 0, 0));

	// Over it, the light spreads past the quad's edges and fades with distance, the same either side.
	ASSERT_TRUE(m_renderer.materials().set(lamp, "color", glm::vec4(4.0f, 2.0f, 1.0f, 1.0f)));
	(void)frame();

	const u32 near = texel(36, 32);
	const u32 far  = texel(44, 32);

	EXPECT_GT(channel(near, 0), channel(far, 0));
	EXPECT_GT(channel(far, 0), 0u);
	EXPECT_EQ(texel(27, 32), near);
	EXPECT_EQ(texel(19, 32), far);

	// The threshold takes the same share of every channel, so the glow keeps the lamp's 4:2:1.
	EXPECT_NEAR(channel(near, 1) * 2.0, channel(near, 0), 2.0);
	EXPECT_NEAR(channel(near, 2) * 4.0, channel(near, 0), 4.0);

	m_screen->set_bloom({.intensity = 0.0f});
	(void)frame();

	EXPECT_EQ(texel(36, 32), rgba(0, 0, 0));
}

TEST_F(Screen, BloomAddsBackItsShareOfTheLight)
{
	// A lamp six units across at 1.25, and a two-level glow whose reach stays inside it: at its centre
	// every level sees nothing but lamp, so the glow there is exactly the light over the threshold,
	// 0.25, however many levels sum to it. Exposure 0.5 brings the result under white.
	(void)glow(glm::vec3(1.25f));
	m_renderer.scene().set_transform(m_objects.back(), glm::scale(glm::mat4(1.0f), glm::vec3(6.0f)));

	m_screen->set_tone(0.5f, 1.0f);
	m_screen->set_bloom({.intensity = 1.0f, .threshold = 1.0f, .levels = 2});
	(void)frame();

	// (1.25 + 0.25) * 0.5 = 0.75.
	EXPECT_EQ(texel(32, 32), rgba(191, 191, 191));

	// Half the intensity, half the glow: (1.25 + 0.125) * 0.5 = 0.6875.
	m_screen->set_bloom({.intensity = 0.5f, .threshold = 1.0f, .levels = 2});
	(void)frame();

	EXPECT_EQ(texel(32, 32), rgba(175, 175, 175));
}

TEST_F(Screen, MaterialsRunInTheOrderListed)
{
	const MaterialTypeHandle invert = type("invert.slang", INVERT_SOURCE);
	const MaterialTypeHandle scale	= type("scale.slang", SCALE_SOURCE);
	MaterialRegistry& materials		= m_renderer.materials();

	const MaterialHandle negative = material(invert);
	const MaterialHandle half	  = material(scale);
	const MaterialHandle quarter  = material(scale); // the type's second record
	ASSERT_TRUE(materials.set(half, "factor", 0.5f));
	ASSERT_TRUE(materials.set(quarter, "factor", 0.25f));

	(void)glow(glm::vec3(0.107f));

	// Inverted, then halved: (1 - 0.107) / 2 = 0.4465.
	m_screen->set_materials({negative, half});
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(114, 114, 114));

	// Halved, then inverted: 1 - 0.107 / 2 = 0.9465.
	m_screen->set_materials({half, negative});
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(241, 241, 241));

	// The same type from its other slot: (1 - 0.107) / 4 = 0.2233.
	m_screen->set_materials({negative, quarter});
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(57, 57, 57));
}

TEST_F(Screen, MaterialsReadTheWorldsDepth)
{
	const MaterialHandle depth = material(type("depth.slang", DEPTH_SOURCE));
	(void)glow(glm::vec3(1.0f), {0.0f, 0.0f, -1.0f});

	m_screen->set_materials({depth});
	(void)frame();

	// Six units from a camera seeing [0.1, 10] in reverse Z: (10 - 6) / 9.9 = 0.404. The clear is
	// the far plane, zero.
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(103, 103, 103));
	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(0, 0, 0));
}

TEST_F(Screen, BlendedMaterialsDrawOverTheImage)
{
	const MaterialHandle wash = material(type("wash.slang", WASH_SOURCE));
	(void)glow(glm::vec3(0.325f));

	m_screen->set_materials({wash});
	(void)frame();

	// A quarter of the way to red: 0.25 + 0.75 * 0.325 = 0.494 red, 0.75 * 0.325 = 0.244 the rest.
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(126, 62, 62));
	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(64, 0, 0));
}

TEST_F(Screen, MaterialsThatCannotDrawAreSkipped)
{
	const MaterialTypeHandle invert = type("invert.slang", INVERT_SOURCE);
	const MaterialHandle gone		= material(invert);
	const MaterialHandle negative	= material(invert);
	m_renderer.materials().destroy(gone);

	(void)glow(glm::vec3(0.2f));

	// A surface material, then a dead one: neither draws, and the chain goes on past them.
	m_screen->set_materials({material(StockType::Unlit), gone, negative});
	(void)frame();

	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(204, 204, 204));
}

TEST_F(Screen, AScreenTypeHotReloadRebuiltDrawsItsNewBuild)
{
	const MaterialTypeHandle effect = type("effect.slang", INVERT_SOURCE);
	const MaterialHandle live		= material(effect);

	(void)glow(glm::vec3(0.2f));
	m_screen->set_materials({live});
	(void)frame();

	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(204, 204, 204));

	// The file now scales, by a factor that defaults to 1.
	material::Type rebuilt;
	String diagnostics;
	ASSERT_TRUE(s_compiler->compile_material("effect.slang", SCALE_SOURCE, rebuilt, diagnostics)) << diagnostics;
	ASSERT_TRUE(m_renderer.materials().replace_type(effect, std::move(rebuilt)));
	(void)frame();

	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(51, 51, 51));
}
