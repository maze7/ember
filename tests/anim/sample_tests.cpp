#include "fixtures.h"

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>
#include <numbers>

using namespace ember;
using namespace ember::anim;
using namespace ember::anim::test;

namespace
{
	constexpr f32 PI = std::numbers::pi_v<f32>;

	// game2's human, sword and slash, as the game's own files describe them.
	constexpr const char* HUMAN_SHEET = R"({
		"image": "textures/human.png", "cell": [24, 24], "pivot": [12, 20],
		"points": { "hand": [12, 12], "heel": [9, 21] },
		"sprites": { "idle": [0, 1], "walk": [2, 3] },
	})";

	constexpr const char* SWORD_SHEET =
		R"({ "image": "textures/iron_sword.png", "cell": [6, 14], "pivot": [3, 10], "sprites": { "blade": [0] } })";

	constexpr const char* SLASH_SHEET =
		R"({ "image": "textures/sword_slash.png", "cell": [33, 18], "pivot": [16, 9], "sprites": { "slash": [0, 1, 2, 3, 4] } })";

	constexpr const char* HUMAN_RIG = R"({
		"slots": { "body": "sheets/human.sheet" },
		"sounds": { "step": "event:/Footstep" },
		"layers": [{ "name": "body" }, { "name": "weapon", "socket": "body.hand", "sort": "y" }],
		"clips": {
			"idle": { "loop": true, "frames": { "body": { "sprites": "idle", "ms": 400 } } },
			"walk": { "loop": true, "frames": { "body": { "sprites": "walk", "ms": 200 } }, "events": [[0, "step"], [200, "step"]] },
			"dash": { "tracks": {
				"body.scale": [[0, [1.6, 0.6]], [866, [1, 1], "expo_out"]],
				"body.y": [[0, -3.2], [866, 0, "expo_out"]],
				"body.flash": [[0, 1], [50, 0, "step"]],
			} },
		},
	})";

	constexpr const char* SWORD_RIG = R"({
		"slots": { "blade": "sheets/sword.sheet", "slash": "sheets/slash.sheet" },
		"sounds": { "slash": "event:/SwordSlash" },
		"turn": "aim",
		"layers": [
			{ "name": "blade", "radius": 6.5, "orbit": -45, "angle": -45, "sprite": "blade" },
			{ "name": "slash", "x": 32, "show": 0 },
		],
		"clips": {
			"rest": {},
			"swing": {
				"length": 300,
				"frames": { "slash": { "sprites": "slash", "ms": 20, "at": 7 } },
				"tracks": {
					"blade.orbit": [[0, "current"], [300, 135, "back_out"]],
					"blade.angle": [[0, -405], [300, -45, "back_out"]],
					"blade.show": [[0, 1], [15, 0], [45, 1]],
					"slash.show": [[0, 0], [7, 1], [107, 0]],
				},
				"events": [[7, "slash"]],
			},
			"swing_back": { "base": "swing", "tracks": {
				"blade.orbit": [[0, "current"], [300, -45, "back_out"]],
				"blade.angle": [[0, 315], [300, -45, "back_out"]],
			} },
		},
	})";

	/** A human with game2's sword, sampled as the game samples it. */
	class Human : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			const u16 body	= m_assets.add(sheet(HUMAN_SHEET, Canvas(96, 24).fill(6, 4, 17, 20).fill(54, 4, 65, 21)));
			const u16 sword = m_assets.add(sheet(SWORD_SHEET, Canvas(6, 14).fill(1, 0, 5, 14)));
			const u16 slash = m_assets.add(sheet(SLASH_SHEET, Canvas(165, 18).fill(0, 0, 33, 18)));

			m_animator.rig_id = m_assets.add(rig(HUMAN_RIG), {body});
			m_look.mount("weapon", "anims/sword.anim");
			m_look.slots[0].id = m_assets.add(rig(SWORD_RIG), {sword, slash});
		}

		[[nodiscard]] Pose at(f64 now, f64 dt = 1.0 / 60.0) const
		{
			return pose_at(m_assets, m_animator, m_look, now, dt);
		}

		/** The part a sheet draws, or null. */
		[[nodiscard]] static const Pose::Part* part(const Pose& pose, u16 sheet)
		{
			for (u32 i = 0; i < pose.part_count; ++i)
				if (pose.parts[i].sheet == sheet)
					return &pose.parts[i];
			return nullptr;
		}

		static constexpr u16 BODY  = 0;
		static constexpr u16 SWORD = 1;
		static constexpr u16 SLASH = 2;

		Assets m_assets;
		Animator m_animator;
		Look m_look;
	};

	/** Two angles the same way round. */
	[[nodiscard]] f32 turn_between(f32 a, f32 b)
	{
		const f32 d = std::remainder(a - b, 2.0f * PI);
		return std::abs(d);
	}

	/**
	 * game2's weapon_system_variable: where its sword is and how it is turned, `t` seconds into a swing
	 * from swipe `from` to swipe `to`, around the middle of the human's cell, 8 texels above the feet.
	 */
	struct Game2Blade
	{
		glm::vec2 position;
		f32 rotation;
	};

	[[nodiscard]] Game2Blade game2_blade(f32 aim, bool face_left, f32 from, f32 to, f32 t)
	{
		const f32 eased = ease(Ease::BackOut, std::min(t / 0.3f, 1.0f));
		const f32 swipe = from + (to - from) * eased;
		const f32 face	= face_left ? -1.0f : 1.0f;
		const f32 orbit = aim + (glm::radians(-45.0f) + 0.5f * (1.0f - swipe) * PI) * face;
		const f32 spin	= (1.0f - eased) * (from < to ? 1.0f : -1.0f) * face * 2.0f * PI;

		return {
			.position = glm::vec2(0.0f, -8.0f) + 6.5f * glm::vec2(std::cos(orbit), std::sin(orbit)),
			.rotation = face_left ? aim + glm::radians(45.0f) + PI + spin : aim + glm::radians(-45.0f) + spin,
		};
	}

	/** game2's visual swipe `t` seconds into a swing: where the next swing, cutting this one short, starts. */
	[[nodiscard]] f32 game2_swipe(f32 from, f32 to, f32 t)
	{
		return from + (to - from) * ease(Ease::BackOut, std::min(t / 0.3f, 1.0f));
	}
}

TEST_F(Human, FlipbookStepsThroughItsSequence)
{
	m_animator.play("walk");

	EXPECT_EQ(part(at(0.0), BODY)->sprite, 2) << "walk_0";
	EXPECT_EQ(part(at(0.199), BODY)->sprite, 2);
	EXPECT_EQ(part(at(0.2), BODY)->sprite, 3) << "walk_1";
	EXPECT_EQ(part(at(0.41), BODY)->sprite, 2) << "a loop starts over";

	m_animator.play("walk", {.since = 0.1});
	EXPECT_EQ(part(at(0.41), BODY)->sprite, 3) << "a clip asked to start later is that much behind";
}

TEST_F(Human, EventsFireOncePerPass)
{
	m_animator.play("walk");

	u32 steps = 0;
	for (u32 frame = 0; frame <= 60; ++frame)
		steps += at(frame / 60.0).fired("step");
	EXPECT_EQ(steps, 6u) << "a foot at 0, 200, 400, 600, 800 and 1000 ms, each once";

	EXPECT_TRUE(at(0.9, 0.8).fired("step")) << "a long frame still passes them";
	EXPECT_FALSE(at(0.15, 0.1).fired("step"));
}

TEST_F(Human, OverlaysCombineThenRunOut)
{
	m_animator.overlay("dash", 1.0);

	const Pose start	   = at(1.0);
	const Pose::Part* body = part(start, BODY);
	EXPECT_FLOAT_EQ(body->scale.x, 1.6f);
	EXPECT_FLOAT_EQ(body->scale.y, 0.6f);
	EXPECT_FLOAT_EQ(body->position.y, -3.2f);
	EXPECT_EQ(body->flash, 1.0f);

	// game2's squash springs back at 8 a second: expo_out over 866 ms is the same curve.
	const Pose later = at(1.25);
	body			 = part(later, BODY);
	EXPECT_NEAR(body->scale.x, 1.0f + 0.6f * std::exp(-8.0f * 0.25f), 1e-3f);
	EXPECT_EQ(body->flash, 0.0f);

	const Pose after = at(2.0);
	body			 = part(after, BODY);
	EXPECT_EQ(body->scale, glm::vec2(1.0f)) << "an overlay that has run its course draws nothing";
	EXPECT_EQ(body->position, glm::vec2(0.0f));
}

TEST_F(Human, AnOverlayAskedForSecondsPlaysItsWholeClipOverThem)
{
	// The squash's 866 ms over the 150 of the dash it is drawn for.
	m_animator.overlay("dash", 1.0, 0.15f);
	EXPECT_FLOAT_EQ(part(at(1.0), BODY)->scale.x, 1.6f);

	const Pose halfway = at(1.075);
	EXPECT_NEAR(part(halfway, BODY)->scale.x, 1.0f + 0.6f * std::exp(-8.0f * 0.433f), 1e-3f)
		<< "half way through its seconds, half way through its clip";

	EXPECT_EQ(part(at(1.151), BODY)->scale, glm::vec2(1.0f)) << "it runs out with them";

	// Asked again without them, it plays at its own length.
	m_animator.overlay("dash", 1.0);
	EXPECT_NEAR(part(at(1.25), BODY)->scale.x, 1.0f + 0.6f * std::exp(-8.0f * 0.25f), 1e-3f);
}

TEST_F(Human, SquashKeepsTheHandWhereGame2HadIt)
{
	m_animator.overlay("dash", 0.0);
	const Pose pose = at(0.0);
	EXPECT_NEAR(glm::distance(*pose.point("body.hand"), glm::vec2(0.0f, -8.0f)), 0.0f, 1e-5f)
		<< "squashed about the cell's middle, as game2 did, the hand stays put";
}

TEST_F(Human, FacingWestMirrors)
{
	m_animator.face({-1.0f, 0.0f});

	const Pose pose = at(0.0);
	EXPECT_TRUE(part(pose, BODY)->mirror);
	EXPECT_EQ(*pose.point("body.heel"), glm::vec2(3.0f, 1.0f)) << "the heel trails, now to the east";
}

TEST_F(Human, MountedRigsSortAboveTheirSocketBehind)
{
	m_animator.set("aim", -PI / 2.0f); // up: the blade rests above the hand
	EXPECT_LT(part(at(0.0), SWORD)->order, part(at(0.0), BODY)->order);

	m_animator.set("aim", PI / 2.0f); // down: below it
	EXPECT_GT(part(at(0.0), SWORD)->order, part(at(0.0), BODY)->order);
}

TEST_F(Human, SwordSwingsAsGame2s)
{
	struct Case
	{
		f32 aim;
		const char* clip;
		f32 from, to;
	};

	const Case cases[] = {
		{0.0f, "swing", 1.0f, -1.0f},
		{0.3f, "swing_back", -1.0f, 1.0f},
		{PI - 0.25f, "swing", 1.0f, -1.0f}, // facing west
		{PI + 0.5f, "swing_back", -1.0f, 1.0f},
	};

	for (const Case& c : cases)
	{
		// Swings alternate, so each follows the other kind, long finished here.
		m_animator = {.rig_id = m_animator.rig_id};
		m_animator.face({std::cos(c.aim), std::sin(c.aim)});
		m_animator.set("aim", c.aim);
		m_animator.play("weapon", c.to < 0.0f ? "swing_back" : "swing", {.since = -1.0});
		m_animator.play("weapon", c.clip, {.since = 0.0});

		for (f32 t = 0.0f; t <= 0.5f; t += 0.01f)
		{
			const Pose pose			= at(t);
			const Pose::Part* sword = part(pose, SWORD);
			const bool hidden		= t >= 0.015f && t < 0.045f;
			const Game2Blade blade	= game2_blade(c.aim, std::cos(c.aim) < 0.0f, c.from, c.to, t);

			ASSERT_EQ(sword == nullptr, hidden) << c.clip << " at " << t << ": gone while the slash smears it";
			if (sword == nullptr)
				continue;

			EXPECT_NEAR(glm::distance(sword->position, blade.position), 0.0f, 1e-3f) << c.clip << " at " << t;
			EXPECT_NEAR(turn_between(sword->angle, blade.rotation), 0.0f, 1e-3f) << c.clip << " at " << t;
			EXPECT_EQ(sword->mirror, std::cos(c.aim) < 0.0f);
		}
	}
}

TEST_F(Human, SwingsCutShortCarryOnAsGame2s)
{
	// As fast as game2 attacks, 12 ticks apart: each swing starts where the one it cut short had the blade.
	const f32 starts[] = {0.0f, 0.2f, 0.4f};
	f32 from[3]		   = {1.0f};
	const f32 to[3]	   = {-1.0f, 1.0f, -1.0f};
	for (u32 i = 1; i < 3; ++i)
		from[i] = game2_swipe(from[i - 1], to[i - 1], starts[i] - starts[i - 1]);

	m_animator.set("aim", 0.0f);
	for (u32 i = 0; i < 3; ++i)
	{
		m_animator.play("weapon", to[i] < 0.0f ? "swing" : "swing_back", {.since = starts[i]});

		for (f32 t = starts[i] + 0.05f; t < starts[i] + 0.2f; t += 0.01f)
		{
			const Game2Blade blade	= game2_blade(0.0f, false, from[i], to[i], t - starts[i]);
			const Pose pose			= at(t);
			const Pose::Part* sword = part(pose, SWORD);
			ASSERT_NE(sword, nullptr);
			EXPECT_NEAR(glm::distance(sword->position, blade.position), 0.0f, 1e-3f) << "swing " << i << " at " << t;
			EXPECT_NEAR(turn_between(sword->angle, blade.rotation), 0.0f, 1e-3f) << "swing " << i << " at " << t;
		}
	}
}

TEST_F(Human, SlashPlaysAlongTheAim)
{
	m_animator.set("aim", PI / 2.0f);
	m_animator.play("weapon", "swing", {.since = 0.0});

	EXPECT_EQ(part(at(0.0), SLASH), nullptr) << "from 7 ms";
	const Pose pose = at(0.03);
	ASSERT_NE(part(pose, SLASH), nullptr);
	EXPECT_EQ(part(pose, SLASH)->sprite, 1) << "its second frame, 20 ms a frame from 7";
	EXPECT_NEAR(glm::distance(part(pose, SLASH)->position, glm::vec2(0.0f, -8.0f + 32.0f)), 0.0f, 1e-4f);
	EXPECT_TRUE(at(0.01).fired("weapon.slash")) << "a mounted rig's events are named through its slot";
}

TEST_F(Human, AnInputPlaysAClipInPlaceOfTheClock)
{
	// game2's walk, played by the distance walked: once through every 62 texels, 31 to each 200 ms frame.
	m_animator.rig_id = m_assets.add(rig(R"({
		"slots": { "body": "sheets/human.sheet" },
		"layers": [{ "name": "body" }],
		"clips": { "walk": { "loop": true, "input": "distance", "span": 62,
							 "frames": { "body": { "sprites": "walk", "ms": 200 } }, "events": [[0, "step"], [200, "step"]] } },
	})"),
									 {BODY});
	m_animator.play("walk");

	Pose pose;
	const auto walk = [&](f64 now, f32 distance)
	{
		m_animator.set("distance", distance);
		sample(m_assets, m_animator, m_look, now, now - 1.0 / 60.0, pose);
		return pose.parts[0].sprite;
	};

	EXPECT_EQ(walk(0.0, 0.0f), 2);
	EXPECT_FALSE(pose.fired("step")) << "first seen, it has passed nothing";
	EXPECT_EQ(walk(5.0, 30.0f), 2) << "the clock does not move it";
	EXPECT_FALSE(pose.fired("step"));
	EXPECT_EQ(walk(5.1, 32.0f), 3) << "past 31 texels, the next frame";
	EXPECT_TRUE(pose.fired("step"));
	EXPECT_EQ(walk(9.0, 32.0f), 3);
	EXPECT_FALSE(pose.fired("step")) << "standing still, it fires nothing, however long";

	u32 steps = 0;
	for (u32 texel = 33; texel <= 652; ++texel)
	{
		(void)walk(10.0 + texel / 60.0, static_cast<f32>(texel));
		steps += pose.fired("step");
	}
	EXPECT_EQ(steps, 20u) << "a foot at 62, 93 ... 651 texels, each once";

	EXPECT_EQ(walk(30.0, 0.0f), 2) << "a new walk starts over";
	EXPECT_FALSE(pose.fired("step")) << "and going back passes nothing";
	EXPECT_EQ(walk(30.1, 40.0f), 3);
	EXPECT_TRUE(pose.fired("step")) << "a long frame still passes its foot";

	Pose fresh;
	m_animator.set("distance", 45.0f);
	sample(m_assets, m_animator, m_look, 40.0, 40.0 - 1.0 / 60.0, fresh);
	EXPECT_EQ(fresh.parts[0].sprite, 3);
	EXPECT_FALSE(fresh.fired("step")) << "first seen part way in, it fires nothing it did not pass";
}

TEST_F(Human, AnEventSoundsAsItsRigSays)
{
	m_animator.play("walk");

	// The pose carries what a passed event sounds like, as the hash audio knows the event by.
	Pose pose = at(0.2);
	ASSERT_TRUE(pose.fired("step"));
	ASSERT_EQ(pose.sound_count, 1u);
	EXPECT_EQ(pose.sounds[0], hash_text("event:/Footstep"));

	EXPECT_EQ(at(0.3).sound_count, 0u) << "a frame that passes no event makes no sound";

	// A mounted rig brings its own: the sword says what its slash sounds like, whoever holds it.
	m_animator.play("idle");
	m_animator.play("weapon", "swing", {.since = 1.0});
	pose = at(1.01);
	ASSERT_TRUE(pose.fired("weapon.slash"));
	ASSERT_EQ(pose.sound_count, 1u);
	EXPECT_EQ(pose.sounds[0], hash_text("event:/SwordSlash"));
}
