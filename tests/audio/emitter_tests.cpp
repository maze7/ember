#include "banks.h"

#include <ember/audio/systems.h>

#include <optional>

using namespace ember;
using namespace ember::audio;
using namespace ember::audio::test;

namespace
{
	/// Where a game keeps its entities.
	struct Place
	{
		glm::vec2 at = {};
	};
	EMBER_COMPONENT(Place, Client);

	/// A fire: it crackles for as long as it is there. A prefab is a constant, its sound a literal.
	inline constexpr auto CAMPFIRE = ecs::prefab("campfire", Place{}, Emitter{.loop = "event:/Ambient"});

	inline constexpr auto WALKER = ecs::prefab("walker", Place{}, Emitter{});

	/** A client's world with the audio systems, and an engine that plays to nowhere in step with it. */
	class Sounding : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			EMBER_NEEDS_BANKS();
			ASSERT_TRUE(start(m_engine));

			m_registry.prefabs(CAMPFIRE, WALKER);
			m_registry.components<anim::Pose>();
			register_systems<&Place::at>(m_registry);

			m_world.emplace(m_registry);
			m_world->add_resource<anim::Clock>();
			add_resources(*m_world, m_engine);
		}

		void TearDown() override { m_world.reset(); }

		/** One frame at `now`: what Present plays, counted before the engine's update starts the next. */
		[[nodiscard]] Stats frame(f64 now)
		{
			m_world->resource<anim::Clock>().set(now);
			m_world->run(ecs::Phase::Present);
			const Stats stats = m_engine.stats();
			m_engine.update();
			return stats;
		}

		[[nodiscard]] Emitter& emitter(ecs::Entity entity) { return m_world->registry.get<Emitter>(entity); }

		Engine m_engine;
		ecs::Registry m_registry;
		std::optional<ecs::World> m_world;
	};

	constexpr f64 TICK = 1.0 / 60.0;
}

TEST_F(Sounding, ASoundIsHeardOnceForItsMoment)
{
	const ecs::Entity walker = m_world->spawn(WALKER);

	// A Present system says it every frame, as it says which clip plays.
	u32 heard = 0;
	for (u32 i = 0; i < 30; ++i)
	{
		emitter(walker).play("event:/Dodge", 1.0);
		heard += frame(1.0 + i * TICK).started;
	}
	EXPECT_EQ(heard, 1u);

	// A later moment is another dash.
	emitter(walker).play("event:/Dodge", 2.0);
	EXPECT_EQ(frame(2.0).started, 1u);
}

TEST_F(Sounding, ASoundWaitsForItsMoment)
{
	const ecs::Entity walker = m_world->spawn(WALKER);

	// What a remote player did shows a little before its moment on this client's clock.
	emitter(walker).play("event:/Dodge", 1.05);
	EXPECT_EQ(frame(1.0).started, 0u);
	emitter(walker).play("event:/Dodge", 1.05);
	EXPECT_EQ(frame(1.04).started, 0u);
	emitter(walker).play("event:/Dodge", 1.05);
	EXPECT_EQ(frame(1.06).started, 1u);
}

TEST_F(Sounding, ASoundLongPastIsNotPlayed)
{
	const ecs::Entity walker = m_world->spawn(WALKER);

	// An entity met with a dash a second old on it: seen mid-way, never heard starting.
	emitter(walker).play("event:/Dodge", 4.0);
	EXPECT_EQ(frame(5.0).started, 0u);
	emitter(walker).play("event:/Dodge", 4.0);
	EXPECT_EQ(frame(5.0 + TICK).started, 0u);
}

TEST_F(Sounding, APredictionPutRightMakesNoSecondSound)
{
	const ecs::Entity walker = m_world->spawn(WALKER);

	emitter(walker).play("event:/SwordSlash", 3.0);
	EXPECT_EQ(frame(3.0).started, 1u);

	// The server had the swing a tick later.
	emitter(walker).play("event:/SwordSlash", 3.0 + TICK);
	EXPECT_EQ(frame(3.0 + 2 * TICK).started, 0u);

	// And then says there was no swing at all: the state is back to the swing before.
	emitter(walker).play("event:/SwordSlash", 2.0);
	EXPECT_EQ(frame(3.0 + 3 * TICK).started, 0u);

	// The next real swing sounds.
	emitter(walker).play("event:/SwordSlash", 3.4);
	EXPECT_EQ(frame(3.4).started, 1u);
}

TEST_F(Sounding, AnEntityMakesMoreSoundsThanItRemembers)
{
	const ecs::Entity walker = m_world->spawn(WALKER);
	const Event events[] = {"event:/Dodge", "event:/SwordSlash", "event:/Footstep", "event:/Ambient", "event:/Dodge"};

	// Five in turn through four places: each still heard once.
	u32 heard = 0;
	for (u32 i = 0; i < 5; ++i)
	{
		const f64 now = 1.0 + i * 0.5;
		emitter(walker).play(events[i], now);
		heard += frame(now).started;
		emitter(walker).play(events[i], now);
		heard += frame(now + TICK).started;
	}
	EXPECT_EQ(heard, 5u);
}

TEST_F(Sounding, AnEmitterIsWhereItsEntityIs)
{
	const ecs::Entity walker = m_world->spawn(WALKER, Place{.at = {100.0f, 40.0f}});
	(void)frame(1.0);
	EXPECT_EQ(emitter(walker).position, glm::vec2(100.0f, 40.0f));

	m_world->registry.get<Place>(walker).at = {0.0f, 50.0f};
	(void)frame(1.0 + TICK);
	EXPECT_EQ(emitter(walker).position, glm::vec2(0.0f, 50.0f));
}

TEST_F(Sounding, AHoldLastsWhileItIsAsked)
{
	const ecs::Entity walker = m_world->spawn(WALKER);

	for (u32 i = 0; i < 10; ++i)
	{
		emitter(walker).hold("event:/Ambient");
		m_world->registry.get<Place>(walker).at = {static_cast<f32>(i), 0.0f};
		(void)frame(i * TICK);
		EXPECT_EQ(m_engine.stats().held, 1u);
	}

	// A frame goes by without it.
	(void)frame(10 * TICK);
	EXPECT_EQ(m_engine.stats().held, 0u);
	EXPECT_EQ(emitter(walker).hold_count, 0u);

	// Asked again, it starts again.
	emitter(walker).hold("event:/Ambient");
	(void)frame(11 * TICK);
	EXPECT_EQ(m_engine.stats().held, 1u);
}

TEST_F(Sounding, AHoldThatPlaysOutDoesNotStartAgainUntilItIsLetGo)
{
	const ecs::Entity walker = m_world->spawn(WALKER);

	// 700 ms of sound held for two seconds: once.
	for (u32 i = 0; i < 100; ++i)
	{
		emitter(walker).hold("event:/Dodge");
		(void)frame(i * TICK);
	}
	EXPECT_EQ(m_engine.stats().held, 0u);
	EXPECT_EQ(emitter(walker).hold_count, 1u);

	(void)frame(101 * TICK);
	emitter(walker).hold("event:/Dodge");
	(void)frame(102 * TICK);
	EXPECT_EQ(m_engine.stats().held, 1u);
}

TEST_F(Sounding, APrefabsLoopPlaysForItsEntitysLife)
{
	const ecs::Entity fire = m_world->spawn(CAMPFIRE);
	(void)frame(0.0);
	(void)frame(TICK);
	EXPECT_EQ(m_engine.stats().held, 1u);

	m_world->registry.destroy(fire);
	EXPECT_EQ(m_engine.stats().held, 0u);
}

TEST_F(Sounding, AWorldTakesItsSoundsWithIt)
{
	(void)m_world->spawn(CAMPFIRE);
	(void)m_world->spawn(CAMPFIRE);
	(void)frame(0.0);
	EXPECT_EQ(m_engine.stats().held, 2u);

	// The editor's Stop: the session goes, and its world with it.
	m_world.reset();
	EXPECT_EQ(m_engine.stats().held, 0u);
}

TEST_F(Sounding, AHoldWhoseBankHasNotArrivedStartsWhenItDoes)
{
	m_engine.unload_bank(bank("Master.bank"));

	(void)m_world->spawn(CAMPFIRE);
	(void)frame(0.0);
	(void)frame(TICK);
	EXPECT_EQ(m_engine.stats().held, 0u);

	ASSERT_TRUE(m_engine.load_bank(bank("Master.bank")));
	(void)frame(2 * TICK);
	EXPECT_EQ(m_engine.stats().held, 1u);
}

TEST_F(Sounding, AHoldComesBackWhenItsBankIsBuiltAgain)
{
	(void)m_world->spawn(CAMPFIRE);
	(void)frame(0.0);
	ASSERT_EQ(m_engine.stats().held, 1u);

	// Studio builds the bank again: what played from the old one stops with it, and did not play out.
	ASSERT_TRUE(m_engine.load_bank(bank("Master.bank")));
	EXPECT_EQ(m_engine.stats().held, 0u);

	(void)frame(TICK);
	(void)frame(2 * TICK);
	EXPECT_EQ(m_engine.stats().held, 1u);
}

TEST_F(Sounding, ASystemPlaysWhatItSeesHappen)
{
	Sounds& sounds = m_world->resource<Sounds>();
	sounds.play("event:/SwordSlash", glm::vec2(3.0f, 4.0f));

	const ParamValue surface{.param = "Surface", .value = 3.0f};
	sounds.play("event:/Footstep", glm::vec2(3.0f, 4.0f), {&surface, 1});
	EXPECT_EQ(m_engine.stats().started, 2u);
}

TEST(Emitter, IsPlainDataAPrefabCarries)
{
	static_assert(std::is_trivially_copyable_v<Emitter>);
	static_assert(ecs::ClientComponent<Emitter>);

	// Names are numbers by the time the program runs.
	static_assert(Event("event:/Ambient").id == hash_text("event:/Ambient"));
	static_assert(std::get<Emitter>(CAMPFIRE.components).loop == Event::from("event:/Ambient"));

	constexpr Emitter asked = []
	{
		Emitter emitter;
		emitter.play("event:/Dodge", 1.0);
		emitter.play("event:/Dodge", 1.0);
		emitter.hold("event:/Ambient");
		emitter.set("Surface", 2.0f);
		return emitter;
	}();
	static_assert(asked.shot_count == 1 && asked.hold_count == 1 && asked.param_count == 1);
	EXPECT_LE(sizeof(Emitter), 320u);
}

TEST(Emitter, AskedWithNoEngineItRemembersAndNothingBreaks)
{
	// A build without FMOD, or a machine where it did not start: the same systems, and nothing is heard.
	Engine silent;
	ecs::Registry registry;
	registry.prefabs(CAMPFIRE, WALKER);
	registry.components<anim::Pose>();
	register_systems<&Place::at>(registry);

	ecs::World world(registry);
	world.add_resource<anim::Clock>().set(1.0);
	add_resources(world, silent);

	const ecs::Entity walker = world.spawn(WALKER);
	(void)world.spawn(CAMPFIRE);
	world.registry.get<Emitter>(walker).play("event:/Dodge", 1.0);
	world.registry.get<Emitter>(walker).hold("event:/Ambient");

	world.run(ecs::Phase::Present);
	world.run(ecs::Phase::Present);
	EXPECT_TRUE(world.registry.get<Emitter>(walker).shots[0].done);
	EXPECT_EQ(silent.stats().held, 0u);
}
