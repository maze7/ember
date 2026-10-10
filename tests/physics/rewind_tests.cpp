#include <ember/core/bitmask.h>
#include <ember/physics/space.h>
#include <ember/physics/systems.h>

#include <gtest/gtest.h>

#include <initializer_list>
#include <utility>

using namespace ember;
using namespace ember::physics;

namespace
{
	enum class Layer : u32
	{
		None   = 0,
		Player = 1 << 0,
		Enemy  = 1 << 1,
	};
	EMBER_ENUM_BITWISE_OPS(Layer, u32);

	constexpr ecs::Entity STRIKER = static_cast<ecs::Entity>(1);
	constexpr ecs::Entity TARGET  = static_cast<ecs::Entity>(2);
	constexpr ecs::Entity OTHER	  = static_cast<ecs::Entity>(3);

	struct Body
	{
		ecs::Entity entity = TARGET;
		glm::vec2 at	   = {};
		Layer layer		   = Layer::Player;
	};

	/** One tick's sync: these hurtboxes, 10 a side, where they stand, and the striker's sword if it swings. */
	void sync(Space& space, std::initializer_list<Body> bodies, const Hitbox* sword = nullptr)
	{
		space.clear();
		for (const Body& body : bodies)
			space.add(body.entity, body.at, Hurtbox{.shape = box({10.0f, 10.0f}), .layer = body.layer});
		if (sword != nullptr)
			space.add(STRIKER, {0.0f, 0.0f}, *sword);
		space.build();
	}

	/** What a shape at a point finds, `ticks` back. */
	[[nodiscard]] Vector<Touch> found(const Space& space, glm::vec2 at, f32 ticks, Layer layers = Layer::Player)
	{
		Vector<Touch> out{&memory::heap(MemoryTag::Physics)};
		space.hurtboxes(box({2.0f, 2.0f}, at), layers, ticks, out);
		return out;
	}

	/** A target walking east, 10 a tick: x = 0, 10, 20 ... over ticks 1, 2, 3 ... */
	void walk(Space& space, u32 ticks)
	{
		for (u32 tick = 1; tick <= ticks; ++tick)
			sync(space, {{.at = {static_cast<f32>(tick - 1) * 10.0f, 0.0f}}});
	}
}

TEST(Rewind, WithoutHistoryItIsThePresent)
{
	Space space;
	walk(space, 6); // the target at 50 now
	EXPECT_EQ(space.history(), 0u);
	EXPECT_TRUE(found(space, {20.0f, 0.0f}, 3.0f).empty()) << "nothing kept: no rewinding";
	ASSERT_EQ(found(space, {50.0f, 0.0f}, 3.0f).size(), 1u) << "the present, whatever the rewind";
}

TEST(Rewind, FindsAHurtboxWhereItStood)
{
	Space space({.history = 8});
	walk(space, 6);
	EXPECT_EQ(space.history(), 5u) << "the five ticks before this one";

	ASSERT_EQ(found(space, {20.0f, 0.0f}, 3.0f).size(), 1u) << "three ticks back it stood at 20";
	EXPECT_EQ(found(space, {20.0f, 0.0f}, 3.0f)[0].entity, TARGET);
	EXPECT_TRUE(found(space, {20.0f, 0.0f}, 0.0f).empty()) << "it is at 50 now";
	EXPECT_TRUE(found(space, {50.0f, 0.0f}, 3.0f).empty()) << "and was not at 50 then";
}

TEST(Rewind, AFractionIsBetweenTwoTicks)
{
	Space space({.history = 8});
	walk(space, 6);

	// 2.5 back is halfway between 30 and 20: a box at 25 +-1 touches it there, and only there.
	const glm::vec2 halfway = {25.0f + 5.0f + 0.5f, 0.0f}; // just inside its east edge at 25
	EXPECT_EQ(found(space, halfway, 2.5f).size(), 1u);
	EXPECT_TRUE(found(space, halfway, 3.0f).empty()) << "at 20 its east edge is at 25";
	EXPECT_EQ(found(space, halfway, 2.0f).size(), 1u) << "at 30 it covers 25 to 35";
	EXPECT_TRUE(found(space, {36.5f, 0.0f}, 2.5f).empty()) << "halfway, its east edge is at 30";
}

TEST(Rewind, NoFurtherBackThanItKeeps)
{
	Space space({.history = 3});
	walk(space, 10); // at 90 now; 60 three ticks back
	EXPECT_EQ(space.history(), 3u);
	EXPECT_EQ(found(space, {60.0f, 0.0f}, 3.0f).size(), 1u);
	EXPECT_EQ(found(space, {60.0f, 0.0f}, 9.0f).size(), 1u) << "asked for nine back, it gives the oldest it has";
}

TEST(Rewind, LayersAreTheOnesItIsOnNow)
{
	Space space({.history = 4});
	// Two ticks switched off, as a dash's i-frames switch a player off, then on again; another the other way.
	sync(space, {{.at = {0.0f, 0.0f}, .layer = Layer::None}, {.entity = OTHER, .at = {40.0f, 0.0f}}});
	sync(space, {{.at = {0.0f, 0.0f}, .layer = Layer::None}, {.entity = OTHER, .at = {40.0f, 0.0f}}});
	sync(space, {{.at = {0.0f, 0.0f}}, {.entity = OTHER, .at = {40.0f, 0.0f}, .layer = Layer::None}});

	ASSERT_EQ(found(space, {0.0f, 0.0f}, 2.0f).size(), 1u) << "off then, on now: it can be hit";
	EXPECT_EQ(found(space, {0.0f, 0.0f}, 2.0f)[0].layers, Layers(Layer::Player));
	EXPECT_TRUE(found(space, {40.0f, 0.0f}, 2.0f).empty()) << "on then, off now: it is safe";
	EXPECT_TRUE(found(space, {0.0f, 0.0f}, 2.0f, Layer::Enemy).empty()) << "on the hitbox's layers only";
}

TEST(Rewind, WhatHasGoneIsNotFound)
{
	Space space({.history = 4});
	sync(space, {{.at = {0.0f, 0.0f}}, {.entity = OTHER, .at = {40.0f, 0.0f}}});
	sync(space, {{.at = {0.0f, 0.0f}}, {.entity = OTHER, .at = {40.0f, 0.0f}}});
	sync(space, {{.entity = OTHER, .at = {40.0f, 0.0f}}}); // the target broke, and has no hurtbox now

	EXPECT_TRUE(found(space, {0.0f, 0.0f}, 1.0f).empty());
	EXPECT_EQ(found(space, {40.0f, 0.0f}, 1.0f).size(), 1u);
}

TEST(Rewind, NeverBackPastAJump)
{
	Space space({.history = 6});
	sync(space, {{.at = {0.0f, 0.0f}}});
	sync(space, {{.at = {5.0f, 0.0f}}});
	sync(space, {{.at = {500.0f, 0.0f}}}); // respawned far away: put there, not moved there
	sync(space, {{.at = {505.0f, 0.0f}}});

	EXPECT_TRUE(found(space, {5.0f, 0.0f}, 2.0f).empty()) << "where it stood before it jumped is not where it is";
	EXPECT_TRUE(found(space, {0.0f, 0.0f}, 3.0f).empty());
	EXPECT_EQ(found(space, {500.0f, 0.0f}, 1.0f).size(), 1u) << "since the jump it is followed as ever";
	EXPECT_EQ(found(space, {505.0f, 0.0f}, 0.0f).size(), 1u);
}

TEST(Rewind, HitsFindTheTargetWhereTheStrikerSawIt)
{
	// The target walks east past a sword held out over x = 20, 3 a tick; the striker's screen is two ticks behind.
	const auto first_blow = [](f32 rewind)
	{
		Space space({.history = 8});
		Hits hits;
		for (u32 tick = 1; tick <= 20; ++tick)
		{
			const Hitbox sword{.shape = box({4.0f, 4.0f}, {20.0f, 0.0f}), .hits = Layer::Player, .rewind = rewind};
			sync(space, {{.at = {static_cast<f32>(tick - 1) * 3.0f, 0.0f}}}, &sword);
			find_hits(space, hits);
			for (const Hit& hit : hits)
				if (hit.ticks == 1)
					return tick;
		}
		return 0u;
	};

	// In the present it first touches when its east edge reaches the sword's west edge, at 18: x = 13, tick 6.
	// Two ticks behind, the striker saw that at tick 8, and the rewound hit lands then, as its screen showed it.
	EXPECT_EQ(first_blow(0.0f), 6u);
	EXPECT_EQ(first_blow(2.0f), 8u);
}

TEST(Rewind, HitsBeganOncePerTouch)
{
	Space space({.history = 8});
	Hits hits;
	u32 began = 0, touching = 0;
	for (u32 tick = 1; tick <= 12; ++tick)
	{
		const Hitbox sword{.shape = box({8.0f, 8.0f}, {0.0f, 0.0f}), .hits = Layer::Player, .rewind = 3.0f};
		sync(space, {{.at = {0.0f, 0.0f}}}, &sword);
		find_hits(space, hits);
		for (const Hit& hit : hits)
		{
			EXPECT_EQ(hit.hitbox, STRIKER);
			EXPECT_EQ(hit.hurtbox, TARGET);
			began += hit.ticks == 1;
			++touching;
		}
	}
	EXPECT_EQ(began, 1u) << "a still target, touched every tick: one blow";
	EXPECT_EQ(touching, 12u) << "the first tick too, which has nothing kept and judges the present";
}
