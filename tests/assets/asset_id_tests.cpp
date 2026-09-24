#include <ember/assets/asset_id.h>

#include <gtest/gtest.h>

namespace
{
	using ember::asset_id;
	using ember::AssetId;

	TEST(AssetId, IsAConstantForALiteralPath)
	{
		constexpr AssetId id = asset_id("textures/tiles.png");
		static_assert(id != 0);
		EXPECT_EQ(id, asset_id("textures/tiles.png"));
	}

	TEST(AssetId, SeparatorsDoNotChangeTheName)
	{
		EXPECT_EQ(asset_id("textures/tiles.png"), asset_id("textures\\tiles.png"));
	}

	TEST(AssetId, DifferentPathsGetDifferentNames)
	{
		EXPECT_NE(asset_id("textures/tiles.png"), asset_id("textures/tiles2.png"));
		EXPECT_NE(asset_id("a/b"), asset_id("b/a"));
		EXPECT_NE(asset_id(""), asset_id("/"));
	}

	TEST(AssetId, MatchesTheReferenceFnv1a)
	{
		// The 64 bit FNV-1a offset basis for the empty string, and its published test vector for "a".
		EXPECT_EQ(asset_id(""), 0xcbf29ce484222325ull);
		EXPECT_EQ(asset_id("a"), 0xaf63dc4c8601ec8cull);
	}
}
