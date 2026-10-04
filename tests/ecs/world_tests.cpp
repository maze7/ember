#include <ember/ecs/system.h>

#include <gtest/gtest.h>

#include <glm/vec2.hpp>

#include <string>
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

	struct TransformComponent
	{
		glm::vec2 position = {};

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_float(stream, position.x);
			serialize_float(stream, position.y);
			return true;
		}
	};
	EMBER_COMPONENT(TransformComponent, Interpolated | Predicted);

	struct MotionComponent
	{
		glm::vec2 velocity = {};

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_float(stream, velocity.x);
			serialize_float(stream, velocity.y);
			return true;
		}
	};
	EMBER_COMPONENT(MotionComponent, Predicted);

	struct HealthComponent
	{
		u16 current = 50;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, current, 16);
			return true;
		}
	};
	EMBER_COMPONENT(HealthComponent, Replicated);

	struct AiComponent
	{
		f32 speed = 2.0f;
	};
	EMBER_COMPONENT(AiComponent, Server);

	struct SpriteComponent
	{
		u16 frame = 0;
	};
	EMBER_COMPONENT(SpriteComponent, Client);

	/** A status effect: anything can catch fire. */
	struct BurningComponent
	{
		u16 ticks_left = 3;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, ticks_left, 16);
			return true;
		}
	};
	EMBER_COMPONENT(BurningComponent, Replicated);

	struct FireVfxComponent
	{
	};
	EMBER_COMPONENT(FireVfxComponent, Client);

	/** What ran, in order: the systems below write their names here. */
	struct Log
	{
		std::vector<std::string> ran;
	};

	/** What the counting system saw. */
	struct Counts
	{
		u32 simulated = 0;
		u32 viewed	  = 0;
	};

	inline constexpr auto CRAWLER =
		prefab("enemies/crawler", TransformComponent{}, MotionComponent{}, HealthComponent{.current = 30},
			   AiComponent{.speed = 3.0f}, SpriteComponent{.frame = 1});

	// ------------------------------------------------------------------ systems

	void chase_system(Sim<const AiComponent, MotionComponent> hunters, Log& log)
	{
		log.ran.push_back("chase");
		for (auto [entity, ai, motion] : hunters.each())
			motion.velocity = {ai.speed, 0.0f};
	}

	void motion_system(Sim<const MotionComponent, TransformComponent> movers, Log& log)
	{
		log.ran.push_back("motion");
		for (auto [entity, motion, transform] : movers.each())
			transform.position += motion.velocity;
	}

	void after_motion_system(Log& log) { log.ran.push_back("after_motion"); }

	void burning_system(Sim<BurningComponent, HealthComponent> burning, Commands& commands, Log& log)
	{
		log.ran.push_back("burning");
		for (auto [entity, burn, health] : burning.each())
		{
			health.current -= 1;
			if (--burn.ticks_left == 0)
				commands.remove<BurningComponent>(entity);
		}
	}

	void count_system(Sim<const HealthComponent> simulated, View<const HealthComponent> known, Counts& counts)
	{
		counts.simulated = 0;
		counts.viewed	 = 0;
		for ([[maybe_unused]] auto [entity, health] : simulated.each())
			++counts.simulated;
		for ([[maybe_unused]] auto [entity, health] : known.each())
			++counts.viewed;
	}

	void sprite_system(View<const TransformComponent, SpriteComponent> sprites, Log& log)
	{
		log.ran.push_back("sprite");
		for (auto [entity, transform, sprite] : sprites.each())
			sprite.frame = static_cast<u16>(transform.position.x);
	}

	void fire_vfx_system(View<const BurningComponent, Without<FireVfxComponent>> catching,
						 View<const SpriteComponent, Without<BurningComponent>> maybe_alight, Commands& commands)
	{
		for ([[maybe_unused]] auto [entity, burn] : catching.each())
			commands.add(entity, FireVfxComponent{});
		for ([[maybe_unused]] auto [entity, sprite] : maybe_alight.each())
			commands.remove<FireVfxComponent>(entity);
	}

	void input_system(Log& log) { log.ran.push_back("input"); }

	void spawn_system(Commands& commands)
	{
		const Spawned spawned = commands.spawn(CRAWLER, TransformComponent{.position = {7.0f, 0.0f}});
		commands.add(spawned, BurningComponent{.ticks_left = 9});
	}

	/** Shared code names everything; a world keeps what lives in it. */
	void decorated_spawn_system(Commands& commands)
	{
		(void)commands.spawn(CRAWLER, SpriteComponent{.frame = 5}, AiComponent{.speed = 8.0f});
	}

	void present_spawn_system(Commands& commands) { (void)commands.spawn(CRAWLER); }

	void read_counts_system(const Counts& counts, Log& log)
	{
		if (counts.viewed > 0)
			log.ran.push_back("read_counts");
	}

	/** A resource that comes and goes: the level a world is playing. */
	struct Level
	{
		u32 number = 0;
	};

	void read_level_system(const Level* level, Log& log)
	{
		log.ran.push_back(level != nullptr ? "level " + std::to_string(level->number) : "no level");
	}

	void add_and_remove_system(Sim<const HealthComponent> targets, Commands& commands)
	{
		for (auto [entity, health] : targets.each())
		{
			commands.add(entity, BurningComponent{.ticks_left = 1});
			commands.remove<BurningComponent>(entity); // after the add: gone
			commands.remove<MotionComponent>(entity);
			commands.add(entity, MotionComponent{.velocity = {5.0f, 5.0f}}); // after the remove: there
		}
	}

	void later_add_system(Sim<const HealthComponent> targets, Commands& commands)
	{
		for (auto [entity, health] : targets.each())
			commands.add(entity, MotionComponent{.velocity = {9.0f, 9.0f}}); // a later system: the last word
	}

	void destroy_system(Sim<const HealthComponent> targets, Commands& commands)
	{
		for (auto [entity, health] : targets.each())
		{
			commands.destroy(entity);
			commands.add(entity, SpriteComponent{}); // to an entity gone by then: nothing
			commands.destroy(entity);				 // gone already: nothing
		}
	}

	// ------------------------------------------------------------------ a game

	/** A game's registry, and worlds of any role made from it. */
	struct Game
	{
		explicit Game(void (*features)(Registry&))
		{
			registry.components<TransformComponent, MotionComponent, HealthComponent, AiComponent, SpriteComponent,
								BurningComponent, FireVfxComponent>();
			registry.prefabs(CRAWLER);
			features(registry);
		}

		[[nodiscard]] Unique<World> world(Role role = Role::Standalone)
		{
			Unique<World> made = memory::make_unique<World>(MemoryTag::ECS, registry, role);
			made->add_resource<Log>();
			made->add_resource<Counts>();
			return made;
		}

		/** One crawler, made from outside any system. */
		[[nodiscard]] static Entity spawn(World& world)
		{
			(void)world.commands().spawn(CRAWLER);
			world.apply_commands();
			return world.registry.view<const PrefabRef>().front(); // EnTT runs newest first
		}

		[[nodiscard]] const Prefab& crawler() const { return *registry.prefab_types().find(CRAWLER); }

		Registry registry;
	};

	void play(Registry& registry)
	{
		registry.simulate<motion_system>(Stage::Move); // registered before chase, still runs after it
		registry.simulate<chase_system>(Stage::Decide, Where::Server);
		registry.simulate<after_motion_system>(Stage::Move);
		registry.simulate<burning_system>(Stage::Resolve, Where::Server);
		registry.present<sprite_system>();
		registry.present<fire_vfx_system>();
		registry.input<input_system>();
	}

	TEST(World, EachRoleMakesItsShareOfAPrefab)
	{
		Game game(&play);
		Unique<World> server = game.world(Role::Server);
		Unique<World> client = game.world(Role::Client);

		const Entity on_server = server->spawn(CRAWLER);
		EXPECT_TRUE(
			(server->registry.all_of<TransformComponent, MotionComponent, HealthComponent, AiComponent>(on_server)));
		EXPECT_FALSE(server->registry.all_of<SpriteComponent>(on_server)) << "a server draws nothing";
		EXPECT_EQ(server->registry.get<HealthComponent>(on_server).current, 30);

		const Entity on_client = client->spawn(CRAWLER);
		EXPECT_TRUE((
			client->registry.all_of<TransformComponent, MotionComponent, HealthComponent, SpriteComponent>(on_client)));
		EXPECT_FALSE(client->registry.all_of<AiComponent>(on_client)) << "a client has no brains";
		EXPECT_EQ(client->registry.get<SpriteComponent>(on_client).frame, 1);
	}

	TEST(World, StagesRunInOrderAndSystemsInTheOrderTheyWereRegistered)
	{
		Game game(&play);
		Unique<World> server = game.world(Role::Server);
		const Entity crawler = game.spawn(*server);

		server->run(Phase::Simulate);
		EXPECT_EQ(server->resource<Log>().ran,
				  (std::vector<std::string>{"chase", "motion", "after_motion", "burning"}));

		// The crawler chose its speed before it moved: no tick of lag.
		EXPECT_EQ(server->registry.get<TransformComponent>(crawler).position.x, 3.0f);
	}

	TEST(World, WhereSaysWhichWorldsRunASystem)
	{
		Game game(&play);
		Unique<World> server = game.world(Role::Server);
		Unique<World> client = game.world(Role::Client);

		server->run(Phase::Input);
		server->run(Phase::Simulate);
		server->run(Phase::Present);
		EXPECT_EQ(server->resource<Log>().ran,
				  (std::vector<std::string>{"chase", "motion", "after_motion", "burning"}));

		client->run(Phase::Input);
		client->run(Phase::Simulate);
		client->run(Phase::Present);
		EXPECT_EQ(client->resource<Log>().ran, (std::vector<std::string>{"input", "motion", "after_motion", "sprite"}));
	}

	TEST(World, ASimSeesWhatThisWorldSimulatesAndAViewSeesEverything)
	{
		Game game([](Registry& registry) { registry.simulate<count_system>(Stage::Decide); });
		Unique<World> client = game.world(Role::Client);

		// What the server sent, which this client only draws, and what it predicts.
		const Entity remote = client->registry.create();
		client->instantiate(remote, game.crawler());
		const Entity predicted = client->registry.create();
		client->instantiate(predicted, game.crawler());
		client->registry.emplace<Simulated>(predicted);

		client->run(Phase::Simulate);
		EXPECT_EQ(client->resource<Counts>().simulated, 1u);
		EXPECT_EQ(client->resource<Counts>().viewed, 2u);
	}

	TEST(World, ASpawnLandsWhenItsStageEndsWithEverythingAskedOfIt)
	{
		Game game(
			[](Registry& registry)
			{
				registry.simulate<spawn_system>(Stage::Act);
				registry.simulate<count_system>(Stage::Act); // the same stage, after the spawn: too soon to see it
			});
		Unique<World> server = game.world(Role::Server);

		server->run(Phase::Simulate);
		EXPECT_EQ(server->resource<Counts>().viewed, 0u) << "a later system of the same stage";

		auto spawned = server->registry.view<const PrefabRef>();
		ASSERT_EQ(spawned.size(), 1u);
		const Entity entity = spawned.front();
		EXPECT_EQ(server->registry.get<PrefabRef>(entity).id, game.crawler().id);
		EXPECT_EQ(game.crawler().name, "enemies/crawler");
		EXPECT_EQ(server->registry.get<TransformComponent>(entity).position.x, 7.0f) << "set over the prefab's";
		EXPECT_EQ(server->registry.get<HealthComponent>(entity).current, 30) << "the prefab's";
		EXPECT_EQ(server->registry.get<BurningComponent>(entity).ticks_left, 9) << "added before it existed";
		EXPECT_TRUE(server->registry.all_of<Simulated>(entity));

		server->run(Phase::Simulate);
		EXPECT_EQ(server->resource<Counts>().viewed, 1u) << "next tick, everyone sees it";
		EXPECT_EQ(server->registry.view<const PrefabRef>().size(), 2u) << "one a tick: commands land once";
	}

	TEST(World, WhatTheSimulationSpawnsItSimulates)
	{
		Game game(
			[](Registry& registry)
			{
				registry.simulate<spawn_system>(Stage::Act, Where::Client);
				registry.present<present_spawn_system>();
			});

		// On a client, a predicted spawn is simulated where it was made; a presentation one is only drawn.
		Unique<World> client = game.world(Role::Client);
		client->run(Phase::Simulate);
		client->run(Phase::Present);
		EXPECT_EQ(client->registry.view<const PrefabRef>().size(), 2u);
		EXPECT_EQ(client->registry.view<const Simulated>().size(), 1u);

		// From outside any system: the server simulates what it makes, a client does not.
		(void)game.spawn(*client);
		EXPECT_EQ(client->registry.view<const Simulated>().size(), 1u);

		Unique<World> server = game.world(Role::Server);
		(void)game.spawn(*server);
		EXPECT_EQ(server->registry.view<const Simulated>().size(), 1u);
	}

	TEST(World, CommandsLandInTheOrderTheyWereMade)
	{
		Game game(
			[](Registry& registry)
			{
				registry.simulate<add_and_remove_system>(Stage::Resolve);
				registry.simulate<later_add_system>(Stage::Resolve);
			});
		Unique<World> server = game.world(Role::Server);
		const Entity entity	 = game.spawn(*server);

		server->run(Phase::Simulate);
		EXPECT_FALSE(server->registry.all_of<BurningComponent>(entity)) << "added, then removed";
		ASSERT_TRUE(server->registry.all_of<MotionComponent>(entity)) << "removed, then added";
		EXPECT_EQ(server->registry.get<MotionComponent>(entity).velocity, glm::vec2(9.0f, 9.0f))
			<< "the later system's add lands after the earlier one's";
	}

	TEST(World, ACommandForAnEntityThatIsGoneDoesNothing)
	{
		Game game([](Registry& registry) { registry.simulate<destroy_system>(Stage::Cleanup); });
		Unique<World> server = game.world(Role::Server);
		const Entity entity	 = game.spawn(*server);

		server->run(Phase::Simulate);
		EXPECT_FALSE(server->registry.valid(entity));
		EXPECT_EQ(server->registry.view<const SpriteComponent>().size(), 0u);
	}

	TEST(World, AStatusEffectComesAndGoes)
	{
		Game game(
			[](Registry& registry)
			{
				registry.simulate<burning_system>(Stage::Resolve, Where::Server);
				registry.present<fire_vfx_system>();
			});
		Unique<World> server = game.world(Role::Server);
		Unique<World> client = game.world(Role::Client);

		// It walks into fire: a component its prefab never had.
		const Entity crawler = game.spawn(*server);
		server->commands().add(crawler, BurningComponent{.ticks_left = 2});
		server->apply_commands();

		server->run(Phase::Simulate);
		EXPECT_EQ(server->registry.get<HealthComponent>(crawler).current, 29);
		EXPECT_TRUE(server->registry.all_of<BurningComponent>(crawler));

		server->run(Phase::Simulate);
		EXPECT_EQ(server->registry.get<HealthComponent>(crawler).current, 28);
		EXPECT_FALSE(server->registry.all_of<BurningComponent>(crawler)) << "burnt out";

		// A client draws fire while it burns, keyed off the component however it arrived.
		const Entity seen = client->registry.create();
		client->instantiate(seen, game.crawler());
		client->registry.emplace<BurningComponent>(seen);
		client->run(Phase::Present);
		EXPECT_TRUE(client->registry.all_of<FireVfxComponent>(seen));

		client->registry.remove<BurningComponent>(seen);
		client->run(Phase::Present);
		EXPECT_FALSE(client->registry.all_of<FireVfxComponent>(seen));
	}

	TEST(World, ASimOrAViewIsALightWrapperOverAnEnttView)
	{
		// What a system is given wraps EnTT's own view, led by Simulated in a Sim.
		static_assert(std::is_same_v<Sim<const AiComponent, MotionComponent>::EnttView,
									 decltype(std::declval<entt::registry&>()
												  .view<const Simulated, const AiComponent, MotionComponent>())>);
		static_assert(std::is_same_v<View<const HealthComponent, Without<BurningComponent>>::EnttView,
									 decltype(std::declval<entt::registry&>().view<const HealthComponent>(
										 entt::exclude<BurningComponent>))>);

		Game game(&play);
		Unique<World> server = game.world(Role::Server);
		const Entity crawler = game.spawn(*server);

		SystemContext context(*server, nullptr);
		const View<const HealthComponent, Without<BurningComponent>> healthy(
			server->registry.view<const HealthComponent>(entt::exclude<BurningComponent>), context);
		EXPECT_TRUE(healthy.contains(crawler));
		EXPECT_EQ(healthy.entt().size_hint(), 1u);
		EXPECT_EQ(healthy.get<const HealthComponent>(crawler).current, 30);
	}

	TEST(World, AResourceTakenConstIsARead)
	{
		Game game([](Registry& registry) { registry.simulate<read_counts_system>(Stage::Act); });
		const SystemInfo& reader = game.registry.systems()[0];

		ASSERT_EQ(reader.access.size(), 2u);
		EXPECT_EQ(reader.access[0].type, entt::type_hash<Counts>::value());
		EXPECT_EQ(reader.access[0].name, "Counts") << "without its namespace";
		EXPECT_FALSE(reader.access[0].write) << "const Counts&";
		EXPECT_TRUE(reader.access[1].write) << "Log&";
		EXPECT_FALSE(reader.structural);
	}

	TEST(World, AResourceAWorldMayNotHaveIsTakenAsAPointer)
	{
		Game game([](Registry& registry) { registry.simulate<read_level_system>(Stage::Act); });
		const Unique<World> world = game.world();

		world->run(Phase::Simulate);
		world->add_resource<Level>(Level{.number = 3});
		world->run(Phase::Simulate);

		EXPECT_EQ(world->resource<Log>().ran, (std::vector<std::string>{"no level", "level 3"}));

		const SystemInfo& reader = game.registry.systems()[0];
		EXPECT_EQ(reader.access[0].name, "Level");
		EXPECT_FALSE(reader.access[0].write) << "const Level*: a read, as const Level& is";
	}

	TEST(World, TheScheduleKnowsWhatEachSystemTouches)
	{
		Game game(&play);

		const SystemInfo* burning = nullptr;
		const SystemInfo* sprite  = nullptr;
		for (const SystemInfo& system : game.registry.systems())
		{
			if (system.name == "burning_system")
				burning = &system;
			if (system.name == "sprite_system")
				sprite = &system;
		}

		ASSERT_NE(burning, nullptr);
		EXPECT_EQ(burning->phase, Phase::Simulate);
		EXPECT_EQ(burning->stage, static_cast<u8>(Stage::Resolve));
		EXPECT_EQ(burning->where, Where::Server);
		EXPECT_TRUE(burning->structural);
		ASSERT_EQ(burning->access.size(), 3u);
		EXPECT_EQ(burning->access[0].name, "BurningComponent");
		EXPECT_TRUE(burning->access[0].write);
		EXPECT_EQ(burning->access[0].type, entt::type_hash<BurningComponent>::value());
		EXPECT_EQ(burning->access[1].name, "HealthComponent");
		EXPECT_TRUE(burning->access[1].write);
		EXPECT_TRUE(burning->access[2].write) << "Log& writes the log";

		ASSERT_NE(sprite, nullptr);
		EXPECT_EQ(sprite->phase, Phase::Present);
		EXPECT_FALSE(sprite->access[0].write) << "a const Transform";
		EXPECT_TRUE(sprite->access[1].write);
		EXPECT_FALSE(sprite->structural);

		Unique<World> server  = game.world(Role::Server);
		const String schedule = server->describe(Phase::Simulate);
		EXPECT_NE(schedule.find("Simulate.Decide"), String::npos) << schedule;
		EXPECT_NE(schedule.find("Simulate.Resolve"), String::npos) << schedule;
		EXPECT_LT(schedule.find("chase_system"), schedule.find("motion_system")) << schedule;
		EXPECT_EQ(schedule.find("sprite_system"), String::npos) << "Present is another phase";
	}

	TEST(World, AStandaloneWorldIsTheWholeGame)
	{
		Game game(&play);
		Unique<World> world = game.world(); // Standalone unless told otherwise
		EXPECT_EQ(world->role(), Role::Standalone);

		// Every component of a prefab, whatever side it lives on.
		const Entity crawler = world->spawn(CRAWLER);
		EXPECT_TRUE(
			(world->registry.all_of<TransformComponent, MotionComponent, HealthComponent, AiComponent, SpriteComponent>(
				crawler)));
		EXPECT_TRUE(world->registry.all_of<Simulated>(crawler)) << "it simulates everything";

		// Every system, wherever it says it runs, in every phase.
		world->run(Phase::Input);
		world->run(Phase::Simulate);
		world->run(Phase::Present);
		EXPECT_EQ(world->resource<Log>().ran,
				  (std::vector<std::string>{"input", "chase", "motion", "after_motion", "burning", "sprite"}));
		EXPECT_EQ(world->registry.get<SpriteComponent>(crawler).frame, 3) << "drawn where the server's rules moved it";
	}

	TEST(World, ASpawnKeepsWhatLivesInItsWorld)
	{
		Game game([](Registry& registry) { registry.simulate<decorated_spawn_system>(Stage::Act); });

		Unique<World> server = game.world(Role::Server);
		server->run(Phase::Simulate);
		const Entity on_server = server->registry.view<const PrefabRef>().front();
		EXPECT_FALSE(server->registry.all_of<SpriteComponent>(on_server)) << "a server draws nothing";
		EXPECT_EQ(server->registry.get<AiComponent>(on_server).speed, 8.0f);

		Unique<World> client = game.world(Role::Client);
		client->run(Phase::Simulate);
		const Entity on_client = client->registry.view<const PrefabRef>().front();
		EXPECT_EQ(client->registry.get<SpriteComponent>(on_client).frame, 5);
		EXPECT_FALSE(client->registry.all_of<AiComponent>(on_client)) << "a client has no brains";

		Unique<World> alone = game.world();
		alone->run(Phase::Simulate);
		const Entity standalone = alone->registry.view<const PrefabRef>().front();
		EXPECT_EQ(alone->registry.get<SpriteComponent>(standalone).frame, 5);
		EXPECT_EQ(alone->registry.get<AiComponent>(standalone).speed, 8.0f);

		// The same for a world's own spawns and sets.
		const Entity made = server->spawn(CRAWLER, SpriteComponent{.frame = 2}, HealthComponent{.current = 7});
		EXPECT_EQ(server->registry.get<HealthComponent>(made).current, 7);
		server->set(made, SpriteComponent{});
		EXPECT_FALSE(server->registry.all_of<SpriteComponent>(made));
	}

	TEST(World, RolesSayWhereEachKindLives)
	{
		static_assert(lives_in(Kind::Sim, Role::Server) && lives_in(Kind::Sim, Role::Client));
		static_assert(lives_in(Kind::Server, Role::Server) && !lives_in(Kind::Server, Role::Client));
		static_assert(!lives_in(Kind::Client, Role::Server) && lives_in(Kind::Client, Role::Client));
		static_assert(lives_in(Kind::Server, Role::Standalone) && lives_in(Kind::Client, Role::Standalone));
	}
}
