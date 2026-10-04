#include "fixtures.h"

#include <fmt/format.h>

using namespace ember;
using namespace ember::anim;
using namespace ember::anim::test;

TEST(AnimParse, SheetCutsCellsAcrossThenDown)
{
	Canvas canvas(48, 48);
	canvas.fill(26, 30, 30, 44); // inside cell 3, the second row's second cell

	const Sheet cut = sheet(R"({
		"image": "unused.png",
		"cell": [24, 24],
		"pivot": [12, 20],
		"points": { "hand": [12, 12] },
		// sequences of cells; a comment, and a trailing comma, as people write them
		"sprites": { "idle": [0, 1], "walk": [3], },
	})",
							canvas);

	ASSERT_EQ(cut.sprites.size(), 3u);
	EXPECT_EQ(cut.extent.width, 48u);
	EXPECT_EQ(cut.sprites[1].min, glm::vec2(24.0f, 0.0f));
	EXPECT_EQ(cut.sprites[2].min, glm::vec2(24.0f, 24.0f));
	EXPECT_EQ(cut.sprites[2].pivot, glm::vec2(12.0f, 20.0f));
	EXPECT_EQ(cut.sprites[2].opaque, glm::vec4(2.0f, 6.0f, 6.0f, 20.0f)) << "what it covers, from its cell";
	EXPECT_EQ(cut.sprites[0].opaque, glm::vec4(0.0f)) << "an empty cell covers nothing";

	EXPECT_EQ(*find(cut.names, name("idle_1")), 1);
	EXPECT_EQ(*find(cut.names, name("idle")), 0) << "a sequence's name is its first frame";
	EXPECT_EQ(find(cut.sequences, name("idle"))->count, 2);
	EXPECT_EQ(*find(cut.points, name("hand")), glm::vec2(12.0f, 12.0f));

	EXPECT_TRUE(cut.casts_shadow) << "a sheet casts a shadow unless it says";

	const Sheet light = sheet(R"({ "image": "unused.png", "cell": [24, 24], "shadow": false })", canvas);
	EXPECT_FALSE(light.casts_shadow);
}

TEST(AnimParse, SheetRefusesCellsPastItsImage)
{
	const Canvas canvas(6, 14);
	Sheet out;
	String error;

	EXPECT_FALSE(parse_sheet(R"({ "image": "a.png", "cell": [6, 15], "sprites": { "blade": [0] } })", canvas.image(),
							 out, error));
	EXPECT_NE(error.find("does not fit"), String::npos) << error;

	EXPECT_FALSE(parse_sheet(R"({ "image": "a.png", "cell": [6, 7], "sprites": { "blade": [2] } })", canvas.image(),
							 out, error));
	EXPECT_NE(error.find("cell 2"), String::npos) << error;

	String path;
	EXPECT_FALSE(sheet_image(R"({ "cell": [6, 7] })", path, error));
	EXPECT_TRUE(sheet_image(R"({ "image": "textures/iron_sword.png" })", path, error));
	EXPECT_EQ(path, "textures/iron_sword.png");
}

TEST(AnimParse, RigReadsLayersAndClips)
{
	const Rig read = rig(R"({
		"slots": { "body": "sheets/human.sheet" },
		"turn": "aim",
		"layers": [
			{ "name": "body" },
			{ "name": "blade", "slot": "weapon", "socket": "body.hand", "sort": "y", "sprite": "blade",
			  "radius": 6.5, "orbit": -45, "scale": [2, 3] },
		],
		"clips": {
			"walk": {
				"loop": true,
				"frames": { "body": { "sprites": "walk", "ms": 200 } },
				"events": [[200, "step"], [0, "step"]],
			},
			"swing": {
				"base": "walk",
				"length": 300,
				"tracks": {
					"blade.orbit": [[0, "current"], [300, 135, "back_out"]],
					"blade.scale": [[0, [1.2, 0.8]]],
					"blade.show": [[0, 1], [15, 0]],
					"blade.sprite": [[0, "blade"], [50, "smear"]],
				},
			},
		},
	})");

	ASSERT_EQ(read.layers.size(), 2u);
	const Layer& blade = read.layers[1];
	EXPECT_EQ(blade.slot, name("weapon"));
	EXPECT_EQ(blade.parent, 0);
	EXPECT_EQ(blade.socket, name("hand"));
	EXPECT_TRUE(blade.sort_y);
	EXPECT_EQ(blade.rest[static_cast<u32>(Channel::Radius)], 6.5f);
	EXPECT_EQ(blade.rest[static_cast<u32>(Channel::ScaleY)], 3.0f);
	EXPECT_EQ(read.layers[0].slot, name("body")) << "a layer's slot is its name unless it says";
	EXPECT_EQ(read.turn, name("aim"));
	EXPECT_STREQ(read.default_sheet(name("body")), "sheets/human.sheet");

	const Clip& walk = *read.clip(name("walk"));
	EXPECT_TRUE(walk.loop);
	ASSERT_EQ(walk.flipbooks.size(), 1u);
	EXPECT_EQ(walk.flipbooks[0].ms, 200.0f);
	ASSERT_EQ(walk.events.size(), 2u);
	EXPECT_EQ(walk.events[0].ms, 0.0f) << "events in time order";

	const Clip& swing = *read.clip(name("swing"));
	EXPECT_EQ(swing.flipbooks.size(), 1u) << "a clip starts as a copy of its base";
	EXPECT_EQ(swing.events.size(), 2u);
	EXPECT_EQ(swing.length, 300.0f);
	EXPECT_EQ(swing.tracks.size(), 4u) << "orbit, scale's two lanes, show";
	EXPECT_TRUE(swing.keys[swing.tracks[0].first].from_current);
	ASSERT_EQ(swing.sprite_tracks.size(), 1u);
	EXPECT_EQ(swing.sprite_keys[swing.sprite_tracks[0].first + 1].sprite, name("smear"));

	for (const Track& track : swing.tracks)
	{
		if (track.channel == Channel::Show)
		{
			EXPECT_EQ(swing.keys[track.first + 1].ease, Ease::Step) << "show steps, whatever the file says";
		}
	}
}

TEST(AnimParse, RigRefusalsSayWhatToFix)
{
	Rig out;
	String error;

	EXPECT_FALSE(parse_rig("{\n  \"layers\": [\n    { \"name\": \"body\" \n  ]\n}", out, error));
	EXPECT_EQ(error.substr(0, 2), "4:") << "where the text stops parsing: " << error;

	EXPECT_FALSE(parse_rig(R"({ "layers": [{ "name": "a" }], "clips": { "c": { "tracks": { "a.spin": [[0, 1]] } } } })",
						   out, error));
	EXPECT_NE(error.find("the channels are"), String::npos) << error;

	EXPECT_FALSE(parse_rig(R"({ "layers": [{ "name": "a", "socket": "b.hand" }] })", out, error));
	EXPECT_NE(error.find("earlier layer"), String::npos) << error;

	EXPECT_FALSE(
		parse_rig(R"({ "layers": [{ "name": "a" }], "clips": { "b": { "base": "c" }, "c": {} } })", out, error));
	EXPECT_NE(error.find("must come before"), String::npos) << error;

	EXPECT_FALSE(
		parse_rig(R"({ "layers": [{ "name": "a" }], "clips": { "c": { "tracks": { "a.x": [[0, 1, "wobble"]] } } } })",
				  out, error));
	EXPECT_NE(error.find("no ease"), String::npos) << error;

	EXPECT_FALSE(
		parse_rig(R"({ "layers": [{ "name": "a" }], "clips": { "c": { "tracks": { "a.x": [] } } } })", out, error));
	EXPECT_NE(error.find("no keys"), String::npos) << error;

	EXPECT_FALSE(
		parse_rig(R"({ "layers": [{ "name": "a" }], "clips": { "c": { "tracks": { "a.x": [[100, 1], [50, 2]] } } } })",
				  out, error));
	EXPECT_NE(error.find("time order"), String::npos) << error;

	EXPECT_FALSE(parse_rig(
		R"({ "layers": [{ "name": "a" }], "clips": { "c": { "frames": { "a": { "sprites": "s", "ms": 0 } } } } })", out,
		error));
	EXPECT_NE(error.find("longer than 0 ms"), String::npos) << error;

	String layers = R"({ "layers": [)";
	for (u32 i = 0; i <= MAX_LAYERS; ++i)
		layers += fmt::format(R"({{ "name": "l{}" }},)", i);
	layers += "] }";
	EXPECT_FALSE(parse_rig(layers, out, error));
	EXPECT_NE(error.find("at most 16 layers"), String::npos) << error;
}

TEST(AnimParse, AClipsOwnEventsReplaceItsBases)
{
	const Rig read = rig(R"({
		"layers": [{ "name": "a" }],
		"clips": {
			"swing": { "length": 300, "events": [[7, "slash"]] },
			"swing_back": { "base": "swing" },
			"feint": { "base": "swing", "events": [] },
		},
	})");

	EXPECT_EQ(read.clip(name("swing_back"))->events.size(), 1u) << "kept when it lists none";
	EXPECT_TRUE(read.clip(name("feint"))->events.empty()) << "replaced when it lists its own";
}

TEST(AnimParse, AClipAnInputPlaysSaysHowMuchOfIt)
{
	Rig out;
	String error;
	EXPECT_TRUE(parse_rig(R"({ "layers": [{ "name": "a" }], "clips": { "walk": { "input": "distance", "span": 62.4 } } })",
						  out, error))
		<< error;
	EXPECT_EQ(out.clips[0].input, name("distance"));
	EXPECT_FLOAT_EQ(out.clips[0].span, 62.4f);

	EXPECT_FALSE(parse_rig(R"({ "layers": [{ "name": "a" }], "clips": { "walk": { "input": "distance", "span": 0 } } })",
						   out, error));
	EXPECT_NE(error.find("span"), String::npos) << error;
}

TEST(AnimParse, ARigSaysWhatItsEventsSoundLike)
{
	const Rig read = rig(R"({
		"sounds": { "step": "event:/Footstep", "slash": "event:/SwordSlash" },
		"layers": [{ "name": "body" }],
		"clips": { "walk": { "length": 400, "events": [[0, "step"]] } },
	})");

	ASSERT_EQ(read.sounds.size(), 2u);
	const Cue* step = find(read.sounds, name("step"));
	ASSERT_NE(step, nullptr);
	EXPECT_EQ(step->sound, hash_text("event:/Footstep")) << "the hash audio finds the event by";
	EXPECT_EQ(step->path, "event:/Footstep") << "and its path, for messages";

	Rig out;
	String error;
	EXPECT_FALSE(parse_rig(R"({ "sounds": { "step": 3 }, "layers": [{ "name": "body" }] })", out, error));
	EXPECT_NE(error.find("step"), String::npos) << error;
}
