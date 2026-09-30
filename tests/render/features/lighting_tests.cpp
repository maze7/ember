#include "drawing.h"

#include <ember/render/features/lighting.h>

using namespace ember;
using namespace ember::render;
using ember::render::test::Drawing;
using ember::render::test::rgba;

namespace
{
	/// A flat colour under the Standard model, with its roughness and metalness to set.
	constexpr const char* MATTE_SOURCE = R"(
import material;

[Shading("standard")]
struct Matte : IMaterial
{
	[Color] [Default("1 1 1 1")] float4 albedo;
	[Default("1")] float roughness;
	float metallic;

	void surface(SurfaceInput s, inout Surface out)
	{
		out.albedo    = albedo.rgb;
		out.roughness = roughness;
		out.metallic  = metallic;
	}
};
)";

	/// A flat colour under the Pixel model, shaded at every pixel: nothing snaps it to a texel.
	constexpr const char* FLAT_SOURCE = R"(
import material;

[Shading("pixel")]
struct Flat : IMaterial
{
	[Color] [Default("1 1 1 1")] float4 albedo;

	void surface(SurfaceInput s, inout Surface out) { out.albedo = albedo.rgb; }
};
)";

	constexpr glm::vec3 BLACK = {0.0f, 0.0f, 0.0f};
	constexpr glm::vec3 WHITE = {1.0f, 1.0f, 1.0f};
}

// The expected values below are the shading models' arithmetic done by hand, so each test pins the
// model's formula, not merely that light changes something. The target is unorm, so a pixel is
// round(255 * the linear colour).

TEST_F(Drawing, StandardShadesAHeadOnLightByTheBook)
{
	const MaterialTypeHandle matte = type("matte.slang", MATTE_SOURCE);
	MaterialRegistry& materials	   = m_renderer.materials();

	const MaterialHandle rough	= material(matte);
	const MaterialHandle metal	= material(matte);
	const MaterialHandle glossy = material(matte);
	ASSERT_TRUE(materials.set(metal, "metallic", 1.0f));
	ASSERT_TRUE(materials.set(glossy, "roughness", 0.5f));

	(void)object(rough, -2.0f);
	(void)object(metal, 0.0f);
	(void)object(glossy, 2.0f);

	// The light travels down the view axis, so N, L, V and H coincide, with no ambient to add.
	m_lighting->set_ambient(BLACK, BLACK);
	(void)m_renderer.scene().create_light({
		.type	   = LightType::Directional,
		.direction = {0.0f, 0.0f, -1.0f},
		.intensity = 0.5f,
	});
	(void)frame();

	// Head on, D is 1/(pi a^2), the visibility 1/4 and F is f0; the pi folds into the light units.
	// Rough dielectric, a = 1: 0.5 * (1 + 1 * 0.25 * 0.04) = 0.505.
	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(129, 129, 129));

	// Metal: no diffuse, and f0 is the albedo: 0.5 * (1 * 0.25 * 1) = 0.125.
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(32, 32, 32));

	// Roughness 0.5 is alpha 0.25: the same reflection in a lobe sixteen times as tall.
	// 0.5 * (1 + 16 * 0.25 * 0.04) = 0.58.
	EXPECT_EQ(pixel(2.0f, 0.0f), rgba(148, 148, 148));
}

TEST_F(Drawing, PixelStepsEachLightToTheNearestBand)
{
	const MaterialHandle green = material(StockType::Sprite);
	ASSERT_TRUE(m_renderer.materials().set(green, "tint", glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)));
	(void)object(green, 0.0f);

	// N.L is 0.6: the light arrives 53 degrees off the normal.
	m_lighting->set_ambient(BLACK, BLACK);
	(void)m_renderer.scene().create_light({
		.type	   = LightType::Directional,
		.direction = {-0.8f, 0.0f, -0.6f},
		.intensity = 0.8f,
	});

	// Smooth: 0.8 * 0.6 = 0.48.
	m_lighting->set_bands(0, 0.0f);
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(0, 122, 0));

	// Four bands: 0.6 rounds to 0.5, so 0.8 * 0.5 = 0.4.
	m_lighting->set_bands(4, 0.0f);
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(0, 102, 0));
}

TEST_F(Drawing, DitherMixesTheTwoNearestBands)
{
	const MaterialHandle green = material(type("flat.slang", FLAT_SOURCE));
	ASSERT_TRUE(m_renderer.materials().set(green, "albedo", glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)));
	(void)object(green, 0.0f);

	m_lighting->set_ambient(BLACK, BLACK);
	m_lighting->set_bands(4, 1.0f);
	(void)m_renderer.scene().create_light({
		.type	   = LightType::Directional,
		.direction = {-0.8f, 0.0f, -0.6f},
		.intensity = 0.8f,
	});
	(void)frame();

	// 0.6 is 2.4 bands: a pixel rounds up where its Bayer threshold passes 0.6, which is 6 of every
	// 16. The fixture's dither cell is one pixel and the block is 4x4 aligned, so it holds each
	// threshold once, and averages 0.475 against the smooth 0.48: the steps keep the level.
	u32 raised = 0;

	for (u32 row = 28; row < 32; ++row)
	{
		for (u32 column = 32; column < 36; ++column)
		{
			const u32 value = texel(column, row);
			EXPECT_TRUE(value == rgba(0, 102, 0) || value == rgba(0, 153, 0)) << std::hex << value;
			raised += value == rgba(0, 153, 0) ? 1 : 0;
		}
	}

	EXPECT_EQ(raised, 6u);
}

TEST_F(Drawing, AmbientBlendsSkyAndGroundByTheNormal)
{
	const MaterialHandle white = material(StockType::Sprite);

	(void)object(white, -2.0f, 0.0f, quad({0.0f, 1.0f, 0.0f}));
	(void)object(white, 0.0f, 0.0f, quad({0.0f, 0.6f, 0.8f}));
	(void)object(white, 2.0f, 0.0f, quad({0.0f, -1.0f, 0.0f}));

	m_lighting->set_ambient(WHITE, BLACK);
	(void)frame();

	// The blend is the normal's height, mapped to [0, 1]: 1, 0.8 and 0.
	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(255, 255, 255));
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(204, 204, 204));
	EXPECT_EQ(pixel(2.0f, 0.0f), rgba(0, 0, 0));
}

TEST_F(Drawing, PointLightsFallOffToNothingAtTheirRange)
{
	// Sprites shade at their texel centres, and the default texture is one texel, so each quad is
	// lit as its centre is: one exact value per quad.
	const MaterialHandle white = material(StockType::Sprite);

	(void)object(white, 0.0f);
	(void)object(white, -2.0f);
	(void)object(white, 3.5f);

	m_lighting->set_ambient(BLACK, BLACK);
	m_lighting->set_bands(0, 0.0f);

	LightDef bulb{.position = {0.0f, 0.0f, 2.0f}, .range = 4.0f};
	const LightHandle light = m_renderer.scene().create_light(bulb);
	(void)frame();

	// Beneath it, d = 2: (1 - (2/4)^4)^2 / (2^2 + 1) = 0.1758.
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(45, 45, 45));

	// Off to the side, d^2 = 8 and N.L = 0.707: (1 - 0.5^2)^2 / 9 * 0.707 = 0.0442.
	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(11, 11, 11));

	// Past the range, d^2 = 16.25: nothing at all, which is what lets clustering drop the light.
	EXPECT_EQ(pixel(3.5f, 0.0f), rgba(0, 0, 0));

	// Distance counts in source radii, so a source twice the size carries its light twice as far:
	// d = 2 is now one radius, (1 - (2/4)^4)^2 / (1 + 1) = 0.4395. The range still ends it.
	bulb.source_radius = 2.0f;
	m_renderer.scene().set_light(light, bulb);
	(void)frame();

	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(112, 112, 112));
	EXPECT_EQ(pixel(3.5f, 0.0f), rgba(0, 0, 0));
}

TEST_F(Drawing, SpotLightsLightOnlyInsideTheirCone)
{
	const MaterialHandle white = material(StockType::Sprite);

	(void)object(white, 0.0f);
	(void)object(white, -2.0f);

	m_lighting->set_ambient(BLACK, BLACK);
	m_lighting->set_bands(0, 0.0f);
	(void)m_renderer.scene().create_light({
		.type		 = LightType::Spot,
		.position	 = {0.0f, 0.0f, 2.0f},
		.direction	 = {0.0f, 0.0f, -1.0f},
		.range		 = 4.0f,
		.inner_angle = 0.3f,
		.outer_angle = 0.5f,
	});
	(void)frame();

	// On the axis the cone is 1 and the spot is the point light above; 45 degrees off it, past the
	// outer angle, the quad the point light lit to 11 gets nothing.
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(45, 45, 45));
	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(0, 0, 0));
}

TEST_F(Drawing, LightsFollowTheSceneFrameByFrame)
{
	const MaterialHandle white = material(StockType::Sprite);
	(void)object(white, 0.0f);

	m_lighting->set_ambient(BLACK, BLACK);

	LightDef sun{.type = LightType::Directional, .direction = {0.0f, 0.0f, -1.0f}};
	const LightHandle light = m_renderer.scene().create_light(sun);

	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(255, 255, 255));

	sun.intensity = 0.6f;
	m_renderer.scene().set_light(light, sun);
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(153, 153, 153));

	m_renderer.scene().destroy_light(light);
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(0, 0, 0));
}
