#include "drawing.h"

#include <cstring>

using namespace ember;
using namespace ember::render;
using ember::render::test::Drawing;
using ember::render::test::rgba;

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

TEST_F(Drawing, AnObjectsDataFollowsItsMaterialToAnotherType)
{
	const MaterialHandle moving	  = material(StockType::Unlit);
	const RenderObjectHandle card = object(moving, 0.0f);

	f32 frame_rect[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	std::memcpy(frame_rect, m_renderer.scene().instance(card.index).bytes, sizeof(frame_rect));
	EXPECT_EQ(frame_rect[2], 0.0f); // the unlit type has no per-object data

	// The material moves to the sprite type, as a .material saved with another type does. The
	// renderer hands the move to the scene at the next frame, and the card shows its whole texture.
	ASSERT_TRUE(m_renderer.materials().set_type(moving, m_renderer.materials().stock_type(StockType::Sprite)));
	EXPECT_EQ(frame()[bucket(StockType::Sprite)], 1u);

	std::memcpy(frame_rect, m_renderer.scene().instance(card.index).bytes, sizeof(frame_rect));
	EXPECT_EQ(frame_rect[2], 1.0f);
	EXPECT_EQ(frame_rect[3], 1.0f);
}
