#include "drawing.h"

#include <ember/render/features/lighting.h>

#include <glm/common.hpp>

using namespace ember;
using namespace ember::render;
using ember::render::test::Drawing;
using ember::render::test::rgba;

namespace
{
	/// An orthographic camera looking down at the ground from the south at 45 degrees, the way a
	/// top-down game frames it, centred on `target`.
	[[nodiscard]] View camera_at(glm::vec3 target)
	{
		return make_view(glm::lookAt(target + glm::vec3{0.0f, 200.0f, 200.0f}, target, {0.0f, 1.0f, 0.0f}),
						 ortho_reverse_z(160.0f, 90.0f, 1.0f, 1000.0f), {640, 360});
	}

	/// Where a world point lands in a shadow map, in texels.
	[[nodiscard]] glm::vec3 in_map(const View& shadow, u32 resolution, glm::vec3 point)
	{
		const glm::vec4 map = shadow_map_matrix(shadow, resolution) * glm::vec4(point, 1.0f);
		return {map.x * static_cast<f32>(resolution), map.y * static_cast<f32>(resolution), map.z};
	}

	const glm::vec3 SUN = glm::normalize(glm::vec3{0.35f, -1.0f, -0.55f});

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

	/// A card with its left half cut away, the way a sprite's transparent texels cut its outline.
	constexpr const char* HALF_SOURCE = R"(
import material;

[Queue("cutout")] [Shading("pixel")]
struct Half : IMaterial
{
	void surface(SurfaceInput s, inout Surface out)
	{
		if (s.uv.x < 0.5)
			discard;
	}
};
)";
}

TEST(ShadowFit, TheMapHoldsStillAsTheCameraPans)
{
	const ShadowFit fit{.resolution = 1024, .min_height = 0.0f, .max_height = 32.0f};

	View home;
	ASSERT_TRUE(make_shadow_view(camera_at({}), SUN, fit, home));

	for (const glm::vec3 pan :
		 {glm::vec3{0.37f, 0.0f, 0.61f}, glm::vec3{13.5f, 0.0f, -7.25f}, glm::vec3{-250.1f, 0.0f, 101.9f}})
	{
		View moved;
		ASSERT_TRUE(make_shadow_view(camera_at(pan), SUN, fit, moved));

		// Panning only moves what the camera sees, so the map keeps its size, and a point keeps its
		// place inside its texel: the grid is fixed in the world, and shadow edges do not crawl.
		EXPECT_EQ(moved.projection[0][0], home.projection[0][0]);

		const glm::vec3 point = pan + glm::vec3{5.3f, 2.0f, -3.7f};
		const glm::vec3 was	  = in_map(home, fit.resolution, point);
		const glm::vec3 is	  = in_map(moved, fit.resolution, point);

		EXPECT_NEAR(glm::fract(was.x), glm::fract(is.x), 1e-2f);
		EXPECT_NEAR(glm::fract(was.y), glm::fract(is.y), 1e-2f);
	}
}

TEST(ShadowFit, TheMapCoversTheGroundTheCameraSees)
{
	const ShadowFit fit{.resolution = 1024, .min_height = 0.0f, .max_height = 32.0f};
	const View camera = camera_at({});

	View shadow;
	ASSERT_TRUE(make_shadow_view(camera, SUN, fit, shadow));

	// Where the view's corners and centre meet the ground, and the tops of casters above them, all
	// land inside the map.
	const glm::mat4 inverse = glm::inverse(camera.view_projection);

	for (const glm::vec2 ndc : {glm::vec2{-1.0f, -1.0f}, glm::vec2{1.0f, -1.0f}, glm::vec2{-1.0f, 1.0f},
								glm::vec2{1.0f, 1.0f}, glm::vec2{0.0f, 0.0f}})
	{
		const glm::vec4 near = inverse * glm::vec4(ndc, 1.0f, 1.0f);
		const glm::vec4 far	 = inverse * glm::vec4(ndc, 0.0f, 1.0f);
		const glm::vec3 a	 = glm::vec3(near) / near.w;
		const glm::vec3 b	 = glm::vec3(far) / far.w;

		for (const f32 height : {fit.min_height, fit.max_height})
		{
			const glm::vec3 point = a + (b - a) * ((height - a.y) / (b.y - a.y));
			const glm::vec3 map	  = in_map(shadow, fit.resolution, point) / static_cast<f32>(fit.resolution);

			EXPECT_GE(map.x, 0.0f);
			EXPECT_LT(map.x, 1.0f);
			EXPECT_GE(map.y, 0.0f);
			EXPECT_LT(map.y, 1.0f);
		}
	}

	// It draws casters only, from every layer, and has no near plane to lose them behind.
	EXPECT_EQ(shadow.required, ObjectFlags::CastsShadow);
	EXPECT_EQ(shadow.layers, LAYER_ALL);
	EXPECT_TRUE(intersects(shadow.frustum, {SUN * -1.0e6f, 1.0f}));
}

TEST(ShadowFit, NothingToShadowWhenTheCameraSeesNoneOfTheHeights)
{
	View shadow;
	EXPECT_FALSE(make_shadow_view(camera_at({}), SUN, {.min_height = 900.0f, .max_height = 950.0f}, shadow));
}

// The drawings below look down -z at quads facing the camera, lit by a sun travelling along +x and -z:
// a caster one unit in front of the z = 0 plane throws its shadow one unit to the right. The ambient
// is sRGB grey, linear 0.214, so a shadow reads as 55 and the sun's N.L of 0.707 on top as 235.

TEST_F(Drawing, CastersShadowWhatLiesBehindThemFromTheSun)
{
	const MaterialHandle white = material(StockType::Sprite);

	(void)object(white, {-1.0f, 0.0f, 1.0f}); // the caster
	(void)object(white, -2.0f);
	(void)object(white, 0.0f);
	(void)object(white, 1.0f);

	const glm::vec3 grey{0.5f, 0.5f, 0.5f};
	m_lighting->set_ambient(grey, grey);
	(void)m_renderer.scene().create_light({
		.type		  = LightType::Directional,
		.direction	  = {1.0f, 0.0f, -1.0f},
		.casts_shadow = true,
	});
	(void)frame();

	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(55, 55, 55));		// behind the caster, seen from the sun
	EXPECT_EQ(pixel(1.0f, 0.0f), rgba(235, 235, 235));	// past its shadow
	EXPECT_EQ(pixel(-2.0f, 0.0f), rgba(235, 235, 235)); // on the sun's side of it
	EXPECT_EQ(pixel(-1.0f, 0.0f), rgba(235, 235, 235)); // the caster, which does not shadow itself
}

TEST_F(Drawing, ACutoutCastsItsSilhouette)
{
	const MaterialHandle white = material(type("flat.slang", FLAT_SOURCE));
	const MaterialHandle half  = material(type("half.slang", HALF_SOURCE));

	// The card's right half, x in [-1, -0.5], throws its shadow on x in [0, 0.5]. The cut half throws
	// nothing.
	(void)object(half, {-1.0f, 0.0f, 1.0f});
	(void)object(white, 0.0f);

	const glm::vec3 grey{0.5f, 0.5f, 0.5f};
	m_lighting->set_ambient(grey, grey);
	(void)m_renderer.scene().create_light({
		.type		  = LightType::Directional,
		.direction	  = {1.0f, 0.0f, -1.0f},
		.casts_shadow = true,
	});
	(void)frame();

	EXPECT_EQ(pixel(0.25f, 0.0f), rgba(55, 55, 55));
	EXPECT_EQ(pixel(-0.25f, 0.0f), rgba(235, 235, 235));
}

TEST_F(Drawing, OnlyCastersAndOnlyTheLightThatAsksMakeShadows)
{
	const MaterialHandle white = material(StockType::Sprite);

	(void)object(white, {-1.0f, 0.0f, 1.0f}, {}, ObjectFlags::None);
	(void)object(white, 0.0f);

	const glm::vec3 grey{0.5f, 0.5f, 0.5f};
	m_lighting->set_ambient(grey, grey);

	LightDef sun{.type = LightType::Directional, .direction = {1.0f, 0.0f, -1.0f}, .casts_shadow = true};
	const LightHandle light = m_renderer.scene().create_light(sun);

	// An object that does not cast is skipped by the shadow view's cull.
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(235, 235, 235));

	// A caster under a light that does not ask for a shadow shadows nothing either.
	(void)object(white, {-1.0f, 0.0f, 1.0f});
	sun.casts_shadow = false;
	m_renderer.scene().set_light(light, sun);
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(235, 235, 235));

	sun.casts_shadow = true;
	m_renderer.scene().set_light(light, sun);
	(void)frame();
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(55, 55, 55));
}

TEST_F(Drawing, ALayerTheCameraSkipsStillCasts)
{
	const MaterialHandle white = material(StockType::Sprite);

	// The pixel camera leaves the wall faces it can never see out of its view, but their shadows
	// belong in the world: shadow views draw every layer.
	constexpr LayerMask UNSEEN = 1u << 1;
	(void)object(white, {-1.0f, 0.0f, 1.0f}, {}, ObjectFlags::CastsShadow, UNSEEN);
	(void)object(white, 0.0f);
	m_view_layers = LAYER_DEFAULT;

	const glm::vec3 grey{0.5f, 0.5f, 0.5f};
	m_lighting->set_ambient(grey, grey);
	(void)m_renderer.scene().create_light({
		.type		  = LightType::Directional,
		.direction	  = {1.0f, 0.0f, -1.0f},
		.casts_shadow = true,
	});
	(void)frame();

	EXPECT_EQ(pixel(-1.0f, 0.0f), rgba(0, 0, 255)); // the caster is not drawn: the clear shows
	EXPECT_EQ(pixel(0.0f, 0.0f), rgba(55, 55, 55)); // but its shadow is
}
