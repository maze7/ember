#include "banks.h"

#include <chrono>
#include <thread>

using namespace ember;
using namespace ember::audio;
using namespace ember::audio::test;

namespace
{
	class Playing : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			EMBER_NEEDS_BANKS();
			ASSERT_TRUE(start(m_engine));
		}

		/** Mix blocks are 1024 samples at 48 kHz: about 21 ms each. */
		void run(u32 updates)
		{
			for (u32 i = 0; i < updates; ++i)
				m_engine.update();
		}

		Engine m_engine;
	};
}

TEST_F(Playing, ABanksEventsAreKnownByTheirPaths)
{
	EXPECT_EQ(m_engine.stats().banks, 2u);
	EXPECT_EQ(m_engine.stats().events, 4u);
	EXPECT_TRUE(m_engine.has("event:/Footstep"));
	EXPECT_TRUE(m_engine.has(Event::from("event:/SwordSlash")));
	EXPECT_FALSE(m_engine.has("event:/Footstop"));
}

TEST_F(Playing, AnEventNoBankHasIsReportedOnce)
{
	m_engine.play("event:/Footstop");
	m_engine.play("event:/Footstop");
	m_engine.play(Event::from("event:/Nothing"));

	EXPECT_EQ(m_engine.stats().unknown, 2u);
	EXPECT_EQ(m_engine.stats().started, 0u);
}

TEST_F(Playing, AFrameStartsOnlySoManyOfOneEvent)
{
	for (u32 i = 0; i < 10; ++i)
		m_engine.play("event:/Footstep", glm::vec2(static_cast<f32>(i), 0.0f));

	EXPECT_EQ(m_engine.stats().started, m_engine.def().max_same);
	EXPECT_EQ(m_engine.stats().dropped, 10u - m_engine.def().max_same);

	// The next frame has its own budget.
	m_engine.update();
	m_engine.play("event:/Footstep", glm::vec2(0.0f));
	EXPECT_EQ(m_engine.stats().started, 1u);
}

TEST_F(Playing, ASoundOutOfEarshotIsNeverStarted)
{
	m_engine.listen({.position = {0.0f, 0.0f}, .focus = {0.0f, 0.0f}});

	// How far the dodge is heard, counting what its spatialiser overrides its range to. A bank built
	// before FMOD 2.04 does not say, and then nothing is judged out of earshot, however far.
	const f32 range = m_engine.range("event:/Dodge");
	if (range == 0.0f)
	{
		m_engine.play("event:/Dodge", glm::vec2(0.0f, 1.0e6f));
		EXPECT_EQ(m_engine.stats().started, 1u);
		EXPECT_EQ(m_engine.stats().culled, 0u);
		return;
	}

	m_engine.play("event:/Dodge", glm::vec2(range * 0.8f, 0.0f));
	EXPECT_EQ(m_engine.stats().started, 1u);

	m_engine.play("event:/Dodge", glm::vec2(0.0f, range * 1.5f));
	EXPECT_EQ(m_engine.stats().started, 1u);
	EXPECT_EQ(m_engine.stats().culled, 1u);

	// Earshot is measured from the focus, wherever the screen's middle is.
	m_engine.listen({.position = {0.0f, 0.0f}, .focus = {0.0f, range}});
	m_engine.play("event:/Dodge", glm::vec2(0.0f, range * 1.5f));
	EXPECT_EQ(m_engine.stats().started, 2u);

	// A sound with no place is never out of earshot.
	m_engine.play("event:/SwordSlash");
	EXPECT_EQ(m_engine.stats().started, 3u);
}

TEST_F(Playing, RangesAreInWorldUnits)
{
	// Sixteen world units to one of Studio's: a designer's range in tiles is sixteen times as far in the world.
	const f32 range = m_engine.range("event:/Dodge");
	m_engine.shutdown();
	ASSERT_TRUE(start(m_engine, {.unit = 16.0f}));
	EXPECT_FLOAT_EQ(m_engine.range("event:/Dodge"), range * 16.0f);

	EXPECT_EQ(m_engine.range("event:/Ambient"), 0.0f) << "a sound with no place has no range";
	EXPECT_EQ(m_engine.range("event:/Footstop"), 0.0f);
}

TEST_F(Playing, AHeldSoundPlaysUntilItIsStopped)
{
	const Voice voice = m_engine.start("event:/Ambient", {});
	ASSERT_TRUE(voice);

	run(100); // two seconds on: a loop
	EXPECT_TRUE(m_engine.playing(voice));
	EXPECT_EQ(m_engine.stats().held, 1u);

	m_engine.stop(voice, false);
	EXPECT_FALSE(m_engine.playing(voice));
	EXPECT_EQ(m_engine.stats().held, 0u);

	// Stale from here: nothing it is asked does anything.
	m_engine.move(voice, {4.0f, 4.0f});
	m_engine.stop(voice);
}

TEST_F(Playing, AHeldSoundThatPlaysOutIsLetGo)
{
	const Voice voice = m_engine.start("event:/Dodge", {}); // 700 ms long
	ASSERT_TRUE(voice);

	run(10);
	EXPECT_TRUE(m_engine.playing(voice));

	run(60);
	EXPECT_FALSE(m_engine.playing(voice));
	EXPECT_EQ(m_engine.stats().held, 0u);
}

TEST_F(Playing, HoldsAreABudget)
{
	m_engine.shutdown();
	ASSERT_TRUE(start(m_engine, {.held = 2}));

	const Voice a = m_engine.start("event:/Ambient", {});
	const Voice b = m_engine.start("event:/Ambient", {});
	EXPECT_TRUE(a);
	EXPECT_TRUE(b);
	EXPECT_FALSE(m_engine.start("event:/Ambient", {}));

	m_engine.stop(a);
	const Voice c = m_engine.start("event:/Ambient", {});
	EXPECT_TRUE(c);
	EXPECT_NE(c, a); // the place again, and another voice: the old one stays stale
	EXPECT_FALSE(m_engine.playing(a));
}

TEST_F(Playing, MusicIsOneEventAtATime)
{
	m_engine.music("event:/Ambient");
	m_engine.update();
	EXPECT_EQ(m_engine.stats().tracks, 1u);

	// Asked again, nothing changes; asked for none, it stops.
	m_engine.music("event:/Ambient");
	m_engine.update();
	EXPECT_EQ(m_engine.stats().tracks, 1u);

	m_engine.music({});
	m_engine.update();
	EXPECT_EQ(m_engine.stats().tracks, 0u);
}

TEST_F(Playing, MusicAskedForBeforeItsBankStartsWhenTheBankArrives)
{
	m_engine.unload_bank(bank("Master.bank"));
	EXPECT_EQ(m_engine.stats().events, 0u);

	m_engine.music("event:/Ambient");
	run(3);
	EXPECT_EQ(m_engine.stats().tracks, 0u);

	ASSERT_TRUE(m_engine.load_bank(bank("Master.bank")));
	m_engine.update();
	EXPECT_EQ(m_engine.stats().tracks, 1u);
}

TEST_F(Playing, ABankBuiltAgainReloadsUnderWhatHoldsIt)
{
	const Voice voice = m_engine.start("event:/Ambient", {});
	m_engine.music("event:/Ambient");
	run(2);
	ASSERT_TRUE(m_engine.playing(voice));

	// Studio built the bank again: the same file, loaded over the old one.
	ASSERT_TRUE(m_engine.load_bank(bank("Master.bank")));
	EXPECT_EQ(m_engine.stats().events, 4u);

	// What played from the old bank went with it; the music is asked for still, and comes back.
	EXPECT_FALSE(m_engine.playing(voice));
	run(2);
	EXPECT_EQ(m_engine.stats().tracks, 1u);
	EXPECT_TRUE(m_engine.start("event:/Ambient", {}));
}

TEST(Engine, DoesNothingUntilItIsUp)
{
	Engine engine;
	engine.play("event:/Footstep");
	engine.music("event:/Ambient");
	engine.update();

	EXPECT_FALSE(engine.start("event:/Ambient", {}));
	EXPECT_FALSE(engine.has("event:/Footstep"));
	EXPECT_FALSE(engine.load_bank("nowhere/Master.bank"));
	EXPECT_EQ(engine.stats().events, 0u);
}

TEST(Engine, ABankArrivesAFewFramesOnAndNothingWaitsForIt)
{
	EMBER_NEEDS_BANKS();

	// As a game runs it: FMOD on its own threads, playing to nowhere.
	Engine engine;
	ASSERT_TRUE(engine.init({.output = Output::None, .live_update = false}));

	const auto frames_until = [&](auto&& done)
	{
		for (u32 frame = 0; frame < 400; ++frame)
		{
			if (done())
				return frame;
			engine.update();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return 400u;
	};

	// Asked for before any bank: silently, since one may yet come.
	engine.ambience("event:/Ambient");
	engine.play("event:/Dodge");
	engine.update();
	EXPECT_EQ(engine.stats().unknown, 0u);

	// The call does not wait for the disk, in whichever order the banks land.
	const auto before = std::chrono::steady_clock::now();
	ASSERT_TRUE(engine.load_bank(bank("Master.bank")));
	ASSERT_TRUE(engine.load_bank(bank("Master.strings.bank")));
	EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::milliseconds(20));

	EXPECT_LT(frames_until([&] { return engine.stats().loading == 0; }), 400u);
	EXPECT_EQ(engine.stats().banks, 2u);
	EXPECT_EQ(engine.stats().events, 4u);

	// The ambience asked for at the start plays now.
	engine.update();
	EXPECT_EQ(engine.stats().tracks, 1u);

	// Built again under the running game: gone for a few frames, then back, and the ambience with it.
	const Voice held = engine.start("event:/Ambient", {});
	ASSERT_TRUE(held);
	ASSERT_TRUE(engine.load_bank(bank("Master.bank")));
	EXPECT_LT(frames_until([&] { return engine.stats().loading == 0 && engine.has("event:/Ambient"); }), 400u);
	EXPECT_FALSE(engine.playing(held));
	engine.update();
	EXPECT_EQ(engine.stats().tracks, 1u);

	// A file that is no bank is refused, with why, and the rest carry on.
	EXPECT_LT(frames_until([&] { return engine.stats().loading == 0; }), 400u);
	(void)engine.load_bank(bank("no_such.bank"));
	EXPECT_LT(frames_until([&] { return engine.stats().loading == 0; }), 400u);
	EXPECT_EQ(engine.stats().banks, 2u);
	EXPECT_TRUE(engine.has("event:/Footstep"));
}
