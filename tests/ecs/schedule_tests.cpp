#include <ember/ecs/system.h>
#include <ember/jobs/job_system.h>

#include <gtest/gtest.h>

#include <glm/vec2.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	/** The game's stages, in the order a tick runs them. */
	enum class Stage : ember::u8
	{
		Decide,
		Act,
		Move,
		Resolve,
		Cleanup,
		Count
	};
}

namespace ember
{
	EMBER_ENUM_NAMES(Stage, "Decide", "Act", "Move", "Resolve", "Cleanup");
}

namespace
{
	using namespace ember;
	using namespace ember::ecs;

	/** A vector the wire carries whole. */
	template <class Stream> bool serialize_vec2(Stream& stream, glm::vec2& value)
	{
		serialize_float(stream, value.x);
		serialize_float(stream, value.y);
		return true;
	}

	struct PositionComponent
	{
		glm::vec2 value = {};

		template <class Stream> bool serialize(Stream& stream) { return serialize_vec2(stream, value); }
	};
	EMBER_COMPONENT(PositionComponent, Predicted);

	struct VelocityComponent
	{
		glm::vec2 value = {};

		template <class Stream> bool serialize(Stream& stream) { return serialize_vec2(stream, value); }
	};
	EMBER_COMPONENT(VelocityComponent, Predicted);

	struct HealthComponent
	{
		i32 current = 100;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_int(stream, current, -10000, 10000);
			return true;
		}
	};
	EMBER_COMPONENT(HealthComponent, Replicated);

	struct SeekerComponent
	{
		f32 speed = 1.0f;
	};
	EMBER_COMPONENT(SeekerComponent, Sim);

	struct GunComponent
	{
		u32 period	 = 3;
		u32 cooldown = 0;
	};
	EMBER_COMPONENT(GunComponent, Sim);

	struct BulletComponent
	{
		u32 age = 0;
		u32 gun = 0; // the entity that fired it

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, age, 32);
			serialize_bits(stream, gun, 32);
			return true;
		}
	};
	EMBER_COMPONENT(BulletComponent, Replicated);

	struct FollowerComponent
	{
		Entity leader = NO_ENTITY;
	};
	EMBER_COMPONENT(FollowerComponent, Sim);

	struct VisitComponent
	{
		u32 count = 0;
	};
	EMBER_COMPONENT(VisitComponent, Sim);

	/** Two components the game never registers: a system's views still get their storages. */
	struct UnlistedComponent
	{
		u32 value = 0;
	};
	EMBER_COMPONENT(UnlistedComponent, Sim);

	struct ShieldComponent
	{
	};
	EMBER_COMPONENT(ShieldComponent, Sim);

	/** Where the seekers go: every seeker reads them. */
	struct Targets
	{
		glm::vec2 points[3] = {{10.0f, 10.0f}, {60.0f, 90.0f}, {95.0f, 15.0f}};
	};

	inline constexpr auto SEEKER = prefab("seeker", PositionComponent{}, VelocityComponent{}, HealthComponent{},
										  SeekerComponent{.speed = 0.5f}, GunComponent{.period = 3});
	inline constexpr auto BULLET =
		prefab("bullet", PositionComponent{}, VelocityComponent{.value = {1.0f, 0.0f}}, BulletComponent{});
	inline constexpr auto FLARE = prefab("flare", PositionComponent{});

	/** What the tally system counted. */
	struct Tally
	{
		u32 bullets = 0;
		u32 most	= 0;
	};

	// ------------------------------------------------------------------ a battle: split loops, spawns, destroys

	void steer_system(Sim<const SeekerComponent, const PositionComponent, VelocityComponent> seekers,
					  const Targets& targets)
	{
		seekers.parallel_each(
			[&](Entity, const SeekerComponent& seeker, const PositionComponent& position, VelocityComponent& velocity)
			{
				// Toward the nearest target.
				glm::vec2 nearest = position.value;
				f32 best		  = std::numeric_limits<f32>::max();
				for (const glm::vec2 target : targets.points)
				{
					const glm::vec2 to = target - position.value;
					const f32 distance = to.x * to.x + to.y * to.y;
					if (distance < best)
					{
						best	= distance;
						nearest = target;
					}
				}
				velocity.value = (nearest - position.value) * (seeker.speed * 0.01f);
			},
			32);
	}

	void fire_system(Sim<GunComponent, const PositionComponent> guns, Commands&)
	{
		guns.parallel_each(
			[&](Entity gun, GunComponent& weapon, const PositionComponent& position, Commands& shots)
			{
				if (weapon.cooldown > 0)
				{
					--weapon.cooldown;
					return;
				}

				weapon.cooldown		 = weapon.period;
				const Spawned bullet = shots.spawn(BULLET, PositionComponent{position});
				shots.add(bullet, BulletComponent{.age = 0, .gun = entt::to_integral(gun)});
			},
			32);
	}

	void drag_system(Sim<VelocityComponent> movers)
	{
		for (auto [entity, velocity] : movers.each())
			velocity.value *= 0.9f;
	}

	void move_system(Sim<const VelocityComponent, PositionComponent> movers)
	{
		movers.parallel_each([](Entity, const VelocityComponent& velocity, PositionComponent& position)
							 { position.value += velocity.value; });
	}

	void age_system(Sim<BulletComponent> bullets, Commands& commands)
	{
		for (auto [entity, bullet] : bullets.each())
		{
			if (++bullet.age > 4)
				commands.destroy(entity);
		}
	}

	void regen_system(Sim<HealthComponent> healthy)
	{
		healthy.parallel_each([](Entity, HealthComponent& health)
							  { health.current = std::min(health.current + 1, 100); });
	}

	void tally_system(View<const BulletComponent> bullets, Tally& tally)
	{
		tally.bullets = 0;
		for ([[maybe_unused]] auto [entity, bullet] : bullets.each())
			++tally.bullets;
		tally.most = std::max(tally.most, tally.bullets);
	}

	void decay_system(Sim<HealthComponent> healthy, const Tally& tally)
	{
		for (auto [entity, health] : healthy.each())
			health.current -= static_cast<i32>(tally.bullets % 7);
	}

	void battle(Registry& registry)
	{
		registry.simulate<steer_system>(Stage::Decide);
		registry.simulate<fire_system>(Stage::Act);
		registry.simulate<drag_system>(Stage::Move);
		registry.simulate<move_system>(Stage::Move); // after drag: it reads the velocity drag writes
		registry.simulate<age_system>(Stage::Move);	 // beside both: it shares nothing with them
		registry.simulate<regen_system>(Stage::Resolve);
		registry.simulate<tally_system>(Stage::Resolve);
		registry.simulate<decay_system>(Stage::Cleanup);
	}

	// ------------------------------------------------------------------ a game

	/** A game's registry, and server worlds that run it. */
	struct Game
	{
		explicit Game(void (*features)(Registry&))
		{
			registry.components<PositionComponent, VelocityComponent, HealthComponent, SeekerComponent, GunComponent,
								BulletComponent, FollowerComponent, VisitComponent>();
			registry.prefabs(SEEKER, BULLET, FLARE);
			features(registry);
		}

		[[nodiscard]] Unique<World> world(RunMode mode, u64 seed = 0)
		{
			Unique<World> made = memory::make_unique<World>(MemoryTag::ECS, registry, Role::Server);
			made->set_mode(mode, seed);
			made->add_resource<Targets>();
			made->add_resource<Tally>();
			return made;
		}

		/** Seekers in a grid, their guns out of step, made from outside any system. */
		static void seekers(World& world, u32 count)
		{
			for (u32 i = 0; i < count; ++i)
			{
				const PositionComponent at{.value = {static_cast<f32>(i % 25) * 4.0f, static_cast<f32>(i / 25) * 4.0f}};
				(void)world.commands().spawn(SEEKER, at, GunComponent{.period = 2 + i % 5});
			}
			world.apply_commands();
		}

		Registry registry;
	};

	/** The world as one number: every entity's every component, in entity order. */
	template <class T> void mix(u64& hash, World& world)
	{
		std::vector<std::pair<u32, T>> rows;
		for (auto [entity, value] : world.registry.view<const T>().each())
			rows.emplace_back(entt::to_integral(entity), value);
		std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

		for (const auto& [id, value] : rows)
		{
			u8 bytes[sizeof(u32) + sizeof(T)];
			std::memcpy(bytes, &id, sizeof(u32));
			std::memcpy(bytes + sizeof(u32), &value, sizeof(T));
			for (const u8 byte : bytes)
				hash = (hash ^ byte) * 0x100000001b3ull;
		}
	}

	[[nodiscard]] u64 hash_of(World& world)
	{
		u64 hash = 0xcbf29ce484222325ull;
		mix<PositionComponent>(hash, world);
		mix<VelocityComponent>(hash, world);
		mix<HealthComponent>(hash, world);
		mix<GunComponent>(hash, world);
		mix<BulletComponent>(hash, world);
		mix<PrefabRef>(hash, world);
		return hash;
	}

	/** Forty ticks of the battle, run the way mode says. */
	[[nodiscard]] u64 fight(RunMode mode, u64 seed = 0)
	{
		Game game(&battle);
		Unique<World> world = game.world(mode, seed);
		game.seekers(*world, 500);

		for (u32 tick = 0; tick < 40; ++tick)
			world->run(Phase::Simulate);

		EXPECT_GT(world->resource<Tally>().most, 100u) << "the guns fired";
		EXPECT_LT(world->registry.view<const BulletComponent>().size(), 1000u) << "and old bullets went";
		return hash_of(*world);
	}

	TEST(Schedule, EveryModeMakesTheSameWorld)
	{
		const u64 serial = fight(RunMode::Serial);

		for (u64 seed = 1; seed <= 8; ++seed)
			EXPECT_EQ(fight(RunMode::Shuffled, seed), serial) << "Shuffled, seed " << seed;

		for (const u32 workers : {1u, 4u})
		{
			jobs::initialize({.worker_count = workers, .fiber_count = 64});
			EXPECT_EQ(fight(RunMode::Jobs), serial) << "Jobs, " << workers << " workers";
			jobs::shutdown();
		}
	}

	// ------------------------------------------------------------------ the graph

	void integrate_system(Sim<const VelocityComponent, PositionComponent>) {}
	void regen_only_system(Sim<HealthComponent>) {}
	void camera_system(Sim<const PositionComponent>) {}
	void brake_system(Sim<VelocityComponent>) {}
	void report_system(Sim<const PositionComponent, const HealthComponent>) {}

	void stamp_system(Sim<PositionComponent>) {}
	void follow_on_system(Sim<const PositionComponent, VelocityComponent>) {}
	void check_system(Sim<const PositionComponent, const VelocityComponent>) {}

	/** The line describe() gives a system. */
	[[nodiscard]] String line_of(const String& schedule, StringView system)
	{
		const size_t start = schedule.find(String(system) + " ");
		return start == String::npos ? String() : schedule.substr(start, schedule.find('\n', start) - start);
	}

	TEST(Schedule, ASystemWaitsOnlyForWhatItShares)
	{
		Game game(
			[](Registry& registry)
			{
				registry.simulate<integrate_system>(Stage::Move);
				registry.simulate<regen_only_system>(Stage::Move);
				registry.simulate<camera_system>(Stage::Move);
				registry.simulate<brake_system>(Stage::Move);
				registry.simulate<report_system>(Stage::Move);
			});
		Unique<World> server  = game.world(RunMode::Serial);
		const String schedule = server->describe(Phase::Simulate);

		EXPECT_EQ(line_of(schedule, "integrate_system").find("after"), String::npos) << schedule;
		EXPECT_EQ(line_of(schedule, "regen_only_system").find("after"), String::npos) << "nothing shared" << schedule;
		EXPECT_NE(line_of(schedule, "camera_system").find("after: integrate_system (PositionComponent)"), String::npos)
			<< "it reads what integrate writes" << schedule;
		EXPECT_NE(line_of(schedule, "brake_system").find("after: integrate_system (VelocityComponent)"), String::npos)
			<< "it writes what integrate read first" << schedule;
		EXPECT_NE(line_of(schedule, "report_system")
					  .find("after: integrate_system (PositionComponent), regen_only_system (HealthComponent)"),
				  String::npos)
			<< "two parents, and not camera: reading together is no reason to wait" << schedule;
	}

	TEST(Schedule, AWaitALongerChainImpliesIsDropped)
	{
		Game game(
			[](Registry& registry)
			{
				registry.simulate<stamp_system>(Stage::Act);
				registry.simulate<follow_on_system>(Stage::Act);
				registry.simulate<check_system>(Stage::Act);
			});
		Unique<World> server = game.world(RunMode::Serial);
		const String check	 = line_of(server->describe(Phase::Simulate), "check_system");

		EXPECT_NE(check.find("after: follow_on_system (VelocityComponent)"), String::npos) << check;
		EXPECT_EQ(check.find("stamp_system"), String::npos) << "follow_on already waits for stamp" << check;
	}

	void heal_system(Sim<HealthComponent>, View<const HealthComponent>) {}

	TEST(Schedule, ASystemTouchesEachTypeOnce)
	{
		Game game([](Registry& registry) { registry.simulate<heal_system>(Stage::Resolve); });
		const SystemInfo& heal = game.registry.systems()[0];

		ASSERT_EQ(heal.access.size(), 1u);
		EXPECT_TRUE(heal.access[0].write) << "a Sim that writes and a View that reads: a write";
	}

	void unlisted_system(Sim<const UnlistedComponent>, View<const HealthComponent, Without<ShieldComponent>>) {}

	TEST(Schedule, EveryStorageAViewReadsExistsBeforeAnythingRuns)
	{
		Game game([](Registry& registry) { registry.simulate<unlisted_system>(Stage::Decide); });
		Unique<World> server		   = game.world(RunMode::Serial);
		const entt::registry& registry = server->registry;

		EXPECT_NE(registry.storage<UnlistedComponent>(), nullptr) << "never registered, still made";
		EXPECT_NE(registry.storage<ShieldComponent>(), nullptr) << "an excluded type's too";
	}

	// ------------------------------------------------------------------ split loops

	void visit_system(Sim<VisitComponent> visits)
	{
		visits.parallel_each([](Entity, VisitComponent& visit) { ++visit.count; }, 8);
	}

	TEST(Schedule, ASplitLoopVisitsEveryEntityOnce)
	{
		const auto visit = [](RunMode mode)
		{
			Game game([](Registry& registry) { registry.simulate<visit_system>(Stage::Act); });
			Unique<World> server = game.world(mode, 3);

			// Visited, and the loop goes over Simulated's storage, which holds more than the loop's entities:
			// it has to skip some of those, and miss none of its own.
			for (u32 i = 0; i < 1300; ++i)
			{
				const Entity entity = server->registry.create();
				if (i < 1200)
					server->registry.emplace<Simulated>(entity);
				if (i >= 200)
					server->registry.emplace<VisitComponent>(entity);
			}

			server->run(Phase::Simulate);
			for (auto [entity, visit] : server->registry.view<const VisitComponent>().each())
				EXPECT_EQ(visit.count, server->registry.all_of<Simulated>(entity) ? 1u : 0u)
					<< "mode " << static_cast<u32>(mode);
		};

		visit(RunMode::Serial);
		visit(RunMode::Shuffled);
		jobs::initialize({.worker_count = 4, .fiber_count = 64});
		visit(RunMode::Jobs);
		jobs::shutdown();
	}

	void volley_system(Sim<const GunComponent> guns, Commands& commands)
	{
		(void)commands.spawn(FLARE, PositionComponent{.value = {-1.0f, 0.0f}}); // before the loop
		guns.parallel_each(
			[&](Entity gun, const GunComponent&, Commands& shots)
			{
				const Spawned bullet = shots.spawn(BULLET);
				shots.add(bullet, BulletComponent{.age = 0, .gun = entt::to_integral(gun)});
			},
			4);
		(void)commands.spawn(FLARE, PositionComponent{.value = {1.0f, 0.0f}}); // after it
	}

	TEST(Schedule, ASplitLoopsCommandsLandInLoopOrder)
	{
		const auto volley = [](RunMode mode)
		{
			Game game([](Registry& registry) { registry.simulate<volley_system>(Stage::Act); });
			Unique<World> server = game.world(mode, 5);
			game.seekers(*server, 50);

			// The order a plain loop would visit the guns in.
			std::vector<u32> guns;
			for (auto [gun, weapon] : server->registry.view<const Simulated, const GunComponent>().each())
				guns.push_back(entt::to_integral(gun));

			server->run(Phase::Simulate);

			// What the volley made, in the order it was made: entities are numbered as they are created.
			std::vector<Entity> made;
			for (auto [entity, ref] : server->registry.view<const PrefabRef>().each())
			{
				if (ref.id != game.registry.prefab_types().find(SEEKER)->id)
					made.push_back(entity);
			}
			std::sort(made.begin(), made.end(),
					  [](Entity a, Entity b) { return entt::to_integral(a) < entt::to_integral(b); });

			std::vector<u32> shots;
			for (size_t i = 1; i + 1 < made.size(); ++i)
				shots.push_back(server->registry.get<BulletComponent>(made[i]).gun);

			EXPECT_EQ(made.size(), guns.size() + 2);
			EXPECT_EQ(server->registry.get<PositionComponent>(made.front()).value.x, -1.0f) << "first, as asked";
			EXPECT_EQ(server->registry.get<PositionComponent>(made.back()).value.x, 1.0f) << "last, as asked";
			EXPECT_EQ(shots, guns) << "in loop order, whichever range ran first";
		};

		volley(RunMode::Serial);
		volley(RunMode::Shuffled);
		jobs::initialize({.worker_count = 4, .fiber_count = 64});
		volley(RunMode::Jobs);
		jobs::shutdown();
	}

	// ------------------------------------------------------------------ what Shuffled is for

	bool g_note = false; // shared by two systems that never said so: the mistake Shuffled finds

	void leave_note_system(Sim<PositionComponent>) { g_note = true; }

	void read_note_system(Sim<HealthComponent> healthy)
	{
		for (auto [entity, health] : healthy.each())
			health.current += g_note ? 1 : 0;
	}

	void clear_note_system(Sim<GunComponent>) { g_note = false; }

	TEST(Schedule, ShuffledFindsSharingNobodyDeclared)
	{
		const auto health_after = [](RunMode mode, u64 seed)
		{
			Game game(
				[](Registry& registry)
				{
					registry.simulate<leave_note_system>(Stage::Act);
					registry.simulate<read_note_system>(Stage::Act); // shares nothing it declared with the note
					registry.simulate<clear_note_system>(Stage::Cleanup);
				});
			Unique<World> server = game.world(mode, seed);
			game.seekers(*server, 1);

			g_note = false;
			for (u32 tick = 0; tick < 10; ++tick)
				server->run(Phase::Simulate);
			return server->registry.get<HealthComponent>(server->registry.view<const HealthComponent>().front())
				.current;
		};

		const i32 serial = health_after(RunMode::Serial, 0);
		EXPECT_EQ(serial, 110);

		bool found = false;
		for (u64 seed = 1; seed <= 8 && !found; ++seed)
			found = health_after(RunMode::Shuffled, seed) != serial;
		EXPECT_TRUE(found) << "in some order the read ran first";
	}

	void follow_system(Sim<const FollowerComponent, PositionComponent> followers)
	{
		// The mistake: another entity's position, which this loop writes, read past the check get() makes.
		// What it reads depends on which range ran first.
		followers.parallel_each(
			[&](Entity, const FollowerComponent& follower, PositionComponent& position)
			{
				position.value = followers.entt().get<PositionComponent>(follower.leader).value + glm::vec2(1.0f, 0.0f);
			});
	}

	TEST(Schedule, ShuffledFindsASplitLoopLeaningOnAnotherEntity)
	{
		const auto line_after = [](RunMode mode, u64 seed)
		{
			Game game([](Registry& registry) { registry.simulate<follow_system>(Stage::Move); });
			Unique<World> server = game.world(mode, seed);

			// A line, each one following the one made before it.
			Entity leader = NO_ENTITY;
			for (u32 i = 0; i < 16; ++i)
			{
				const Entity entity = server->registry.create();
				server->registry.emplace<Simulated>(entity);
				server->registry.emplace<PositionComponent>(entity);
				server->registry.emplace<FollowerComponent>(entity, leader == NO_ENTITY ? entity : leader);
				leader = entity;
			}

			for (u32 tick = 0; tick < 3; ++tick)
				server->run(Phase::Simulate);
			return hash_of(*server);
		};

		const u64 serial = line_after(RunMode::Serial, 0);
		bool found		 = false;
		for (u64 seed = 1; seed <= 8 && !found; ++seed)
			found = line_after(RunMode::Shuffled, seed) != serial;
		EXPECT_TRUE(found) << "in some order a range read a position before or after another range wrote it";
	}

	// ------------------------------------------------------------------ the job system

	std::atomic<u32> g_arrived{0};

	/** True once two callers are here at the same time; false if the other never came. */
	bool meet() noexcept
	{
		g_arrived.fetch_add(1);
		const auto start = std::chrono::steady_clock::now();
		while (g_arrived.load() < 2 && std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
			std::this_thread::yield();
		return g_arrived.load() >= 2;
	}

	struct Meetings
	{
		std::atomic<u32> met{0};
	};

	void left_system(Sim<PositionComponent>, Meetings& meetings) { meetings.met += meet() ? 1 : 0; }
	void right_system(Sim<HealthComponent>, const Targets&) { (void)meet(); }

	void ranges_meet_system(Sim<VisitComponent> visits)
	{
		visits.parallel_each([](Entity, VisitComponent& visit) { visit.count = meet() ? 1 : 0; }, 1);
	}

	TEST(Schedule, JobsRunWhatSharesNothingAtOnce)
	{
		jobs::initialize({.worker_count = 4, .fiber_count = 64});

		// Two systems that share nothing: each waits until the other is running too.
		{
			Game game(
				[](Registry& registry)
				{
					registry.simulate<left_system>(Stage::Decide);
					registry.simulate<right_system>(Stage::Decide);
				});
			Unique<World> server = game.world(RunMode::Jobs);
			server->add_resource<Meetings>();

			g_arrived = 0;
			server->run(Phase::Simulate);
			EXPECT_EQ(server->resource<Meetings>().met.load(), 1u) << "they ran at the same time";
		}

		// A split loop's ranges likewise.
		{
			Game game([](Registry& registry) { registry.simulate<ranges_meet_system>(Stage::Decide); });
			Unique<World> server = game.world(RunMode::Jobs);
			for (u32 i = 0; i < 2; ++i)
			{
				const Entity entity = server->registry.create();
				server->registry.emplace<Simulated>(entity);
				server->registry.emplace<VisitComponent>(entity);
			}

			g_arrived = 0;
			server->run(Phase::Simulate);
			for (auto [entity, visit] : server->registry.view<const VisitComponent>().each())
				EXPECT_EQ(visit.count, 1u) << "both ranges ran at the same time";
		}

		jobs::shutdown();
	}

	// ------------------------------------------------------------------ what debug builds stop

#ifndef NDEBUG
	void careless_system(Sim<const GunComponent> guns, Commands& commands)
	{
		guns.parallel_each([&](Entity gun, const GunComponent&) { commands.destroy(gun); });
	}

	void unannounced_system(Sim<const GunComponent> guns)
	{
		guns.parallel_each([](Entity gun, const GunComponent&, Commands& commands) { commands.destroy(gun); });
	}

	void nested_system(Sim<const GunComponent> guns)
	{
		guns.parallel_each([&](Entity, const GunComponent&)
						   { guns.parallel_each([](Entity, const GunComponent&) {}); });
	}

	void peeking_system(Sim<const FollowerComponent, PositionComponent> followers)
	{
		followers.parallel_each([&](Entity, const FollowerComponent& follower, PositionComponent& position)
								{ position.value = followers.get<PositionComponent>(follower.leader).value; });
	}

	/** One tick of one system over a few seekers. */
	void run_once(void (*features)(Registry&), RunMode mode = RunMode::Serial)
	{
		Game game(features);
		Unique<World> server = game.world(mode);
		game.seekers(*server, 4);
		server->run(Phase::Simulate);
	}

	TEST(ScheduleDeathTest, ASplitLoopMakesCommandsThroughItsOwnCommands)
	{
		EXPECT_DEATH(run_once([](Registry& registry) { registry.simulate<careless_system>(Stage::Act); }),
					 "make commands with the Commands& it passes fn");
	}

	TEST(ScheduleDeathTest, ASystemWhoseSplitLoopMakesCommandsTakesCommands)
	{
		EXPECT_DEATH(run_once([](Registry& registry) { registry.simulate<unannounced_system>(Stage::Act); }),
					 "split loop makes commands takes Commands& too");
	}

	TEST(ScheduleDeathTest, ASplitLoopDoesNotSplitAgain)
	{
		EXPECT_DEATH(run_once([](Registry& registry) { registry.simulate<nested_system>(Stage::Act); }),
					 "parallel_each inside parallel_each");
	}

	TEST(ScheduleDeathTest, ASplitLoopDoesNotReadWhatItWritesOfAnotherEntity)
	{
		const auto peek = []
		{
			Game game([](Registry& registry) { registry.simulate<peeking_system>(Stage::Move); });
			Unique<World> server = game.world(RunMode::Serial);
			const Entity entity	 = server->registry.create();
			server->registry.emplace<Simulated>(entity);
			server->registry.emplace<PositionComponent>(entity);
			server->registry.emplace<FollowerComponent>(entity, entity);
			server->run(Phase::Simulate);
		};
		EXPECT_DEATH(peek(), "another entity's copy of what the loop writes may be mid-write");
	}

	TEST(ScheduleDeathTest, JobsModeNeedsTheJobSystem)
	{
		EXPECT_DEATH(run_once(&battle, RunMode::Jobs), "runs on the job system");
	}
#endif
}
