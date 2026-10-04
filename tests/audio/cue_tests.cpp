#include "banks.h"

#include "../anim/fixtures.h"

#include <ember/audio/systems.h>

#include <optional>

using namespace ember;
using namespace ember::anim::test;
using namespace ember::audio::test;

namespace
{
	constexpr const char* HUMAN_SHEET = R"({
		"image": "textures/human.png", "cell": [24, 24], "pivot": [12, 20],
		"points": { "hand": [12, 12], "heel": [9, 21] },
		"sprites": { "idle": [0, 1], "walk": [2, 3] },
	})";

	constexpr const char* SWORD_SHEET =
		R"({ "image": "textures/iron_sword.png", "cell": [6, 14], "pivot": [3, 10], "sprites": { "blade": [0] } })";

	constexpr const char* SLASH_SHEET =
		R"({ "image": "textures/sword_slash.png", "cell": [33, 18], "pivot": [16, 9], "sprites": { "slash": [0, 1, 2, 3, 4] } })";

	// The game's rigs, each saying what its events sound like.
	constexpr const char* HUMAN_RIG = R"({
		"slots": { "body": "sheets/human.sheet" },
		"sounds": { "step": "event:/Footstep" },
		"layers": [{ "name": "body" }, { "name": "weapon", "socket": "body.hand", "sort": "y" }],
		"clips": {
			"idle": { "loop": true, "frames": { "body": { "sprites": "idle", "ms": 400 } } },
			"walk": { "loop": true, "frames": { "body": { "sprites": "walk", "ms": 200 } }, "events": [[0, "step"], [200, "step"]] },
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
			"swing": { "length": 300, "frames": { "slash": { "sprites": "slash", "ms": 20, "at": 7 } }, "events": [[7, "slash"]] },
		},
	})";

	struct Place
	{
		glm::vec2 at = {};
	};
	EMBER_COMPONENT(Place, Client);

	inline constexpr auto HUMAN =
		ecs::prefab("human", Place{}, anim::Look{}, anim::Animator{}, anim::Pose{}, audio::Emitter{});

	/** A human with a sword in a client's world: sampled as the game samples it, heard through the engine. */
	class Cued : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			EMBER_NEEDS_BANKS();
			ASSERT_TRUE(start(m_engine));

			const u16 body	= m_assets.add(sheet(HUMAN_SHEET, Canvas(96, 24).fill(6, 4, 17, 20).fill(54, 4, 65, 21)));
			const u16 sword = m_assets.add(sheet(SWORD_SHEET, Canvas(6, 14).fill(1, 0, 5, 14)));
			const u16 slash = m_assets.add(sheet(SLASH_SHEET, Canvas(165, 18).fill(0, 0, 33, 18)));
			const u16 human = m_assets.add(rig(HUMAN_RIG), {body});
			const u16 blade = m_assets.add(rig(SWORD_RIG), {sword, slash});

			m_registry.prefabs(HUMAN);
			audio::register_systems<&Place::at>(m_registry);

			m_world.emplace(m_registry);
			m_world->add_resource<anim::Clock>();
			audio::add_resources(*m_world, m_engine);

			m_human												  = m_world->spawn(HUMAN);
			m_world->registry.get<anim::Animator>(m_human).rig_id = human;

			anim::Look& look = m_world->registry.get<anim::Look>(m_human);
			look.mount("weapon", "anims/sword.anim");
			look.slots[0].id = blade;
		}

		void TearDown() override { m_world.reset(); }

		/** A frame: the pose sampled as anim::animate samples it, then Present, which plays what it passed. */
		[[nodiscard]] audio::Stats frame(f64 now)
		{
			anim::Clock& clock = m_world->resource<anim::Clock>();
			clock.set(now);

			auto& registry = m_world->registry;
			anim::sample(m_assets, registry.get<anim::Animator>(m_human), registry.get<anim::Look>(m_human), clock.now,
						 clock.previous, registry.get<anim::Pose>(m_human));

			m_world->run(ecs::Phase::Present);
			const audio::Stats stats = m_engine.stats();
			m_engine.update();
			return stats;
		}

		audio::Engine m_engine;
		Assets m_assets;
		ecs::Registry m_registry;
		std::optional<ecs::World> m_world;
		ecs::Entity m_human = ecs::NO_ENTITY;
	};
}

TEST_F(Cued, AWalkIsHeardStepByStep)
{
	(void)frame(0.99);
	m_world->registry.get<anim::Animator>(m_human).play("walk", {.since = 1.0});

	// Two steps a 400 ms loop: six in a second, the first as the walk begins.
	u32 steps = 0;
	for (u32 i = 0; i <= 60; ++i)
		steps += frame(1.0 + i / 60.0).started;
	EXPECT_EQ(steps, 6u);
}

TEST_F(Cued, AMountedRigBringsItsOwnSound)
{
	(void)frame(0.99);
	m_world->registry.get<anim::Animator>(m_human).play("weapon", "swing", {.since = 1.0});

	// The slash is 7 ms into the swing: heard on the frame that passes it, once.
	u32 slashes = 0;
	for (u32 i = 0; i < 30; ++i)
		slashes += frame(1.0 + i / 60.0).started;
	EXPECT_EQ(slashes, 1u);
}

TEST_F(Cued, WhatAnEntityStandsOnReachesItsSteps)
{
	// The game says what is underfoot; every step reads it.
	(void)frame(0.99);
	m_world->registry.get<audio::Emitter>(m_human).set("Surface", 3.0f);
	m_world->registry.get<anim::Animator>(m_human).play("walk", {.since = 1.0});

	EXPECT_EQ(frame(1.0).started + frame(1.0 + 1.0 / 60.0).started, 1u);
	EXPECT_FALSE(m_world->registry.get<audio::Emitter>(m_human).params_changed);
}
