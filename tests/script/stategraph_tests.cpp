#include <ember/anim/components.h>
#include <ember/core/hash.h>
#include <ember/ecs/system.h>
#include <ember/net/replication.h>
#include <ember/net/serialize.h>
#include <ember/physics/components.h>
#include <ember/physics/systems.h>
#include <ember/script/components.h>
#include <ember/script/host.h>
#include <ember/script/lua.h>
#include <ember/script/schema.h>
#include <ember/script/triggers.h>

#include <gtest/gtest.h>

#include <glm/vec2.hpp>

#include <initializer_list>
#include <optional>
#include <utility>

/**
 * Stategraphs over entities: states entered, updated and left, timelines whose marks fire on their
 * ticks, events from scripts and from the game, the cap on runaway transitions, the dice and e:play().
 */
namespace stategraph_test
{
	using namespace ember;
	using namespace ember::script;

	struct Position
	{
		glm::vec2 value = {};

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_float(stream, value.x);
			serialize_float(stream, value.y);
			return true;
		}
	};
	EMBER_COMPONENT(Position, Interpolated | Predicted);

	/** Where scripts leave answers: derived, so every context may write it. */
	struct Scratch
	{
		u32 value = 0;
		f64 wide  = 0.0;
		bool flag = false;
	};
	EMBER_COMPONENT(Scratch, Sim);

	enum class Stage : u8
	{
		Act,
		React,
		Count
	};

	inline constexpr auto PLAIN	 = ecs::prefab("plain", Position{}, Scratch{}, anim::Playing{});
	inline constexpr auto BITER	 = ecs::prefab("biter", Position{}, Scratch{},
											   physics::Hitbox{.shape = physics::circle(4.0f)}, stategraphs("biter"));
	inline constexpr auto TARGET =
		ecs::prefab("target", Position{}, physics::Hurtbox{.shape = physics::circle(4.0f), .layer = physics::Layers{}});
	inline constexpr auto WALKER = ecs::prefab("walker", Position{}, Scratch{}, anim::Playing{}, stategraphs("walker"));
	inline constexpr auto SIM_WALKER = ecs::prefab("sim_walker", Position{}, Scratch{}, stategraphs("sim_walker"));

	enum class Layer : u32
	{
		Body = 1u << 0,
	};
	inline constexpr auto BODY = ecs::prefab("body", Position{}, Scratch{},
											 physics::Hurtbox{.shape = physics::circle(4.0f), .layer = Layer::Body});
	inline constexpr auto PIT =
		ecs::prefab("pit", Position{}, Scratch{}, Trigger{.shape = physics::circle(8.0f), .layers = Layer::Body});

	void act_scripts(Host& host, ecs::Commands& commands) { host.simulate(static_cast<u8>(Stage::Act), commands); }
	void react_scripts(Host& host, ecs::Commands& commands) { host.simulate(static_cast<u8>(Stage::React), commands); }

	void bind(Binding& binding)
	{
		binding.expose<Position>();
		binding.expose<Scratch>({.derived = true});
		binding.expose<physics::Hitbox>({.derived = true});
		binding.stage("Act", static_cast<u8>(Stage::Act));
		binding.stage("React", static_cast<u8>(Stage::React));
		binding.units(16.0, 60.0);
	}

	using File = std::pair<const char*, const char*>;

	class StategraphTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			auto parsed = Aliases::parse("scripts", bytes_of(R"({ "aliases": { "lib": "./lib" } })"));
			ASSERT_TRUE(parsed.has_value());
			m_aliases = std::move(*parsed);
		}

		[[nodiscard]] static Span<const u8> bytes_of(StringView text) noexcept
		{
			return {reinterpret_cast<const u8*>(text.data()), text.size()};
		}

		[[nodiscard]] Source make(StringView path, StringView text)
		{
			auto compiled = compile(path, bytes_of(text), m_aliases);
			EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message.c_str());
			if (!compiled)
				return {};
			for (const Problem& lint : compiled->lints)
				ADD_FAILURE() << path << ":" << lint.line << ": " << lint.message;
			return std::move(*compiled);
		}

		void build(std::initializer_list<File> files, ecs::Role role = ecs::Role::Standalone)
		{
			m_registry.prefabs(PLAIN, WALKER, SIM_WALKER, BITER, TARGET, BODY, PIT);
			register_components(m_registry);
			m_registry.simulate_exclusive<&act_scripts>(Stage::Act);
			physics::register_systems<&Position::value>(m_registry, Stage::React); // the hits a strike ends on
			m_registry.simulate<&sense_triggers<&Position::value>>(Stage::React, ecs::Where::Server);
			m_registry.simulate_exclusive<&react_scripts>(Stage::React);
			m_registry.present_exclusive<&run_present>();

			for (const File& file : files)
				m_sources.push_back(make(file.first, file.second));
			m_schema = apply_schema(m_registry, Span<const Source>(m_sources.data(), m_sources.size()), &bind);

			m_world.emplace(m_registry, role);
			physics::add_resources(*m_world);
			m_host = &m_world->add_resource<Host>(*m_world, HostDef{.budget = 20'000});
			bind(m_host->binding());
			m_host->reload(Span<const Source>(m_sources.data(), m_sources.size()));
		}

		void load(std::initializer_list<File> files)
		{
			Vector<Source> sources(&memory::heap(MemoryTag::Scripting));
			for (const File& file : files)
				sources.push_back(make(file.first, file.second));
			m_host->reload(sources);
		}

		void tick(u32 count = 1)
		{
			for (u32 i = 0; i < count; ++i)
			{
				m_host->set_tick(++m_tick);
				m_world->run(ecs::Phase::Simulate);
			}
		}

		[[nodiscard]] const Problem* problem_with(StringView fragment) const
		{
			for (const Problem& problem : m_host->problems())
				if (StringView(problem.message).find(fragment) != StringView::npos)
					return &problem;
			return nullptr;
		}

		[[nodiscard]] Scratch& scratch(ecs::Entity entity) { return m_world->registry.get<Scratch>(entity); }
		[[nodiscard]] const Stategraph::Slot& sg(ecs::Entity entity, u32 slot = 0)
		{
			return m_world->registry.get<Stategraph>(entity).slots[slot];
		}

		ecs::Registry m_registry;
		Vector<Source> m_sources{&memory::heap(MemoryTag::Scripting)};
		Schema m_schema;
		std::optional<ecs::World> m_world;
		Host* m_host = nullptr;
		Aliases m_aliases;
		u32 m_tick = 0;
	};

	TEST_F(StategraphTest, EntersTheInitialStateAndMovesOnLengthUpdateAndEvents)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = {
					a = { enter = function(e) e.Scratch.value = 1 end, length = ticks(3), next = "b" },
					b = {
						enter = function(e) e.Scratch.value = 2 end,
						every = ticks(2),
						update = function(e) e.Scratch.wide += 1 end,
						exit = function(e) e.Scratch.flag = true end,
						events = { done = "c" },
					},
					c = { enter = function(e) e.Scratch.value = 3 end },
				},
			}
		)"}});
		EXPECT_TRUE(m_host->problems().empty()) << (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
		EXPECT_EQ(m_host->stats().stategraphs, 1u);

		const ecs::Entity walker = m_world->spawn(WALKER);
		EXPECT_EQ(sg(walker).state, NO_STATE);
		EXPECT_EQ(m_host->state_of(walker), "");

		tick();
		EXPECT_EQ(m_host->state_of(walker), "a");
		EXPECT_EQ(scratch(walker).value, 1u);
		EXPECT_EQ(sg(walker).since, 1u);
		EXPECT_EQ(sg(walker).state, state_id("a")) << "a state is kept as its name's hash";

		tick(2);
		EXPECT_EQ(m_host->state_of(walker), "a") << "its length is three ticks";
		tick();
		EXPECT_EQ(m_host->state_of(walker), "b");
		EXPECT_EQ(scratch(walker).value, 2u);
		EXPECT_EQ(scratch(walker).wide, 1.0) << "update runs on the tick a state is entered";
		EXPECT_EQ(sg(walker).since, 4u);
		tick();
		EXPECT_EQ(scratch(walker).wide, 1.0) << "every two ticks";
		tick();
		EXPECT_EQ(scratch(walker).wide, 2.0);

		m_host->event(walker, "done");
		tick();
		EXPECT_EQ(m_host->state_of(walker), "c");
		EXPECT_TRUE(scratch(walker).flag) << "b's exit ran";
		EXPECT_EQ(scratch(walker).value, 3u);
		EXPECT_EQ(m_host->stats().transitions, 3u);
		EXPECT_EQ(m_host->stats().events, 1u);

		// A plain entity hears events and ignores them; nothing of it changes.
		const ecs::Entity plain = m_world->spawn(PLAIN);
		m_host->event(plain, "done");
		tick(3);
		EXPECT_EQ(scratch(plain).value, 0u);
		EXPECT_TRUE(m_host->problems().empty());
	}

	TEST_F(StategraphTest, TimelineMarksFireOnTheirTicksAndKnowThem)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = {
					a = {
						timeline = { [0] = "start", [2] = "mid", [4] = "mid", [5] = "stop" },
						on = {
							start = function(e) e.Scratch.value += 1 end,
							mid = function(e) e.Scratch.value += 10 end,
							stop = function(e)
								e.Scratch.wide = e.Stategraph:tick_of("stop")
								e.Scratch.flag = #e.Stategraph:ticks_of("mid") == 2 and e.Stategraph:ticks_of("mid")[2] == e.Stategraph:tick_of("mid") + 2
							end,
						},
						length = ticks(6),
						next = "a",
					},
				},
			}
		)"}});
		const ecs::Entity walker = m_world->spawn(WALKER);
		tick();
		EXPECT_EQ(scratch(walker).value, 1u) << "the mark at 0 fires on entry";
		tick();
		EXPECT_EQ(scratch(walker).value, 1u);
		tick();
		EXPECT_EQ(scratch(walker).value, 11u) << "elapsed 2";
		tick(2);
		EXPECT_EQ(scratch(walker).value, 21u) << "elapsed 4";
		tick();
		EXPECT_EQ(scratch(walker).wide, 6.0) << "tick_of: since 1 + 5";
		EXPECT_TRUE(scratch(walker).flag);
		tick();
		EXPECT_EQ(scratch(walker).value, 22u) << "its length up, it re-enters itself: the start mark again";
		EXPECT_EQ(sg(walker).since, 7u);
		EXPECT_TRUE(m_host->problems().empty()) << (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, ReactHandlersGetTheSourceAndScriptsRaiseEvents)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "idle",
				states = {
					idle = {
						react = {
							poke = function(e, source)
								if source then e.Scratch.wide = source:id() end
								return "poked"
							end,
						},
					},
					poked = { enter = function(e) e.Scratch.value = 7 end, events = { calm = "idle" } },
				},
			}
		)"},
			   {"scripts/server/rules/poke.luau", R"(
			system("Act", function()
				for e, s in world:query(Scratch) do
					if s.flag then
						s.flag = false
						e:event("poke", e)
					end
				end
			end)
		)"}});
		const ecs::Entity walker = m_world->spawn(WALKER);
		tick();
		EXPECT_EQ(m_host->state_of(walker), "idle");

		// Raised by a system after the point's graphs ran: the graph answers at its next point, a tick on.
		scratch(walker).flag = true;
		tick();
		EXPECT_EQ(m_host->state_of(walker), "idle");
		tick();
		EXPECT_EQ(m_host->state_of(walker), "poked");
		EXPECT_EQ(scratch(walker).value, 7u);
		EXPECT_EQ(scratch(walker).wide, static_cast<f64>(entt::to_integral(walker))) << "the source came along";

		m_host->event(walker, "calm");
		tick();
		EXPECT_EQ(m_host->state_of(walker), "idle");
		EXPECT_TRUE(m_host->problems().empty()) << (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, RunawayTransitionsAndUnknownStatesStopTheGraph)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = {
					a = { update = function(e) e.Scratch.value += 1; return "b" end },
					b = { update = function(e) return "a" end },
				},
			}
		)"},
			   {"scripts/server/stategraphs/sim_walker.luau", R"(
			stategraph "sim_walker" { initial = "a", states = { a = { update = function() return "zzz" end } } }
		)"}});
		const ecs::Entity walker = m_world->spawn(WALKER);
		const ecs::Entity other	 = m_world->spawn(SIM_WALKER);
		tick();
		ASSERT_NE(problem_with("more than 8 transitions in one tick"), nullptr);
		EXPECT_EQ(problem_with("more than 8 transitions")->path, "scripts/server/stategraphs/walker.luau");
		ASSERT_NE(problem_with("update returned 'zzz', which is not a state"), nullptr);
		const u32 counted = scratch(walker).value;
		EXPECT_GE(counted, 4u);

		tick(3);
		EXPECT_EQ(scratch(walker).value, counted) << "off until its module reloads";
		EXPECT_EQ(m_host->state_of(other), "a");
		EXPECT_EQ(m_host->stats().errors, 2u);

		load({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = { a = { update = function(e) e.Scratch.value += 1 end }, b = { update = function(e) e.Scratch.value += 1 end } },
			}
		)"}});
		tick();
		EXPECT_EQ(scratch(walker).value, counted + 1) << "the reload put it back to work, in the state it was in";
	}

	TEST_F(StategraphTest, GoMovesTheGraphFromItsOwnHandlersOnly)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = {
					a = {
						enter = function(e) e.Scratch.value = 1 end,
						update = function(e) if e.Scratch.flag then e.Stategraph:go("b") end end,
						exit = function(e) e.Scratch.wide = 5 end,
					},
					b = { enter = function(e) e.Scratch.value = 2 end },
				},
			}
		)"},
			   {"scripts/server/rules/meddle.luau", R"(
			system("React", function()
				for e, s in world:query(Scratch) do
					if s.value == 2 then e.Stategraph:go("a") end
				end
			end)
		)"}});
		const ecs::Entity walker = m_world->spawn(WALKER);
		tick();
		EXPECT_EQ(m_host->state_of(walker), "a");
		scratch(walker).flag = true;
		tick();
		EXPECT_EQ(m_host->state_of(walker), "b") << "go from a handler: left and entered in the same tick";
		EXPECT_EQ(scratch(walker).value, 2u);
		EXPECT_EQ(scratch(walker).wide, 5.0) << "a's exit ran";
		ASSERT_NE(problem_with("Stategraph:go is for the graph's own handlers"), nullptr);
		EXPECT_EQ(problem_with("Stategraph:go is for")->path, "scripts/server/rules/meddle.luau");
	}

	TEST_F(StategraphTest, ServerGraphsAndSystemsRunOnServerWorldsOnly)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" { initial = "a", states = { a = { enter = function(e) e.Scratch.value = 1 end } } }
			system("Act", function() for e, s in world:query(Scratch) do s.wide = 1 end end)
		)"},
			   {"scripts/sim/stategraphs/sim_walker.luau", R"(
			stategraph "sim_walker" { initial = "a", states = { a = { enter = function(e) e.Scratch.flag = true end } } }
		)"}},
			  ecs::Role::Client);
		EXPECT_TRUE(m_host->problems().empty()) << (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());

		// Simulated here, as a predicted entity is; a server graph still never runs on a client.
		const ecs::Entity walker = m_world->create(m_world->prefab(WALKER), true);
		const ecs::Entity simmed = m_world->create(m_world->prefab(SIM_WALKER), true);
		const ecs::Entity shown	 = m_world->create(m_world->prefab(SIM_WALKER), false);
		tick();
		EXPECT_EQ(m_host->state_of(walker), "") << "a server graph on a client";
		EXPECT_EQ(scratch(walker).value, 0u);
		EXPECT_EQ(scratch(walker).wide, 0.0) << "a server system on a client";
		EXPECT_EQ(m_host->state_of(simmed), "a") << "a sim graph on what the client simulates";
		EXPECT_TRUE(scratch(simmed).flag);
		EXPECT_EQ(m_host->state_of(shown), "") << "not on what it only draws";
	}

	TEST_F(StategraphTest, DiceAreTheSameForTheSameTickEntityAndRoll)
	{
		build({{"scripts/server/rules/dice.luau", R"(
			system("Act", function()
				for e, s in world:query(Scratch) do
					s.wide = e.rng:range(1000) * 1000 + e.rng:range(1000)
					s.value = e.rng:range(1)
					local f = e.rng:float()
					s.flag = f >= 0 and f < 1 and e.rng:chance(1) and not e.rng:chance(0)
				end
			end)
		)"}});
		const ecs::Entity a = m_world->spawn(PLAIN);
		const ecs::Entity b = m_world->spawn(PLAIN);
		tick();
		const f64 first_a = scratch(a).wide;
		const f64 first_b = scratch(b).wide;
		EXPECT_NE(first_a, first_b) << "two entities roll differently";
		EXPECT_EQ(scratch(a).value, 0u) << "range(1) is always 0";
		EXPECT_TRUE(scratch(a).flag);
		tick();
		EXPECT_NE(scratch(a).wide, first_a) << "the next tick rolls afresh";

		// Another world of the same game, at the same tick, with the same entities: the same numbers.
		ecs::World twin(m_registry);
		physics::add_resources(twin);
		Host& host = twin.add_resource<Host>(twin, HostDef{});
		bind(host.binding());
		host.reload(Span<const Source>(m_sources.data(), m_sources.size()));
		const ecs::Entity twin_a = twin.spawn(PLAIN);
		(void)twin.spawn(PLAIN);
		host.set_tick(1);
		twin.run(ecs::Phase::Simulate);
		EXPECT_EQ(twin.registry.get<Scratch>(twin_a).wide, first_a);
		EXPECT_TRUE(m_host->problems().empty());
	}

	TEST_F(StategraphTest, PlayWritesWhatTheClientsDraw)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" { initial = "a", states = { a = { enter = function(e) e:play("squat") end } } }
		)"},
			   {"scripts/sim/rules/wrong.luau", R"(
			system("Act", function() for e, s in world:query(Scratch) do e:play("idle") end end)
		)"}});
		const ecs::Entity walker = m_world->spawn(WALKER);
		tick();
		EXPECT_EQ(m_world->registry.get<anim::Playing>(walker).clip, anim::name("squat"));
		EXPECT_EQ(m_world->registry.get<anim::Playing>(walker).since, 1u);
		ASSERT_NE(problem_with("Playing.play: a sim script writes Predicted components only"), nullptr);

		const ecs::Entity bare = m_world->registry.create();
		m_world->registry.emplace<Scratch>(bare);
		m_world->registry.emplace<ecs::Simulated>(bare);
		load({{"scripts/server/rules/right.luau", "system(\"Act\", function() for e, s in world:query(Scratch) do e:play(\"idle\") end end)"}});
		tick();
		ASSERT_NE(problem_with("the entity has no Playing component"), nullptr);
	}

	TEST_F(StategraphTest, MistakesInADeclarationAreNamedAndTheModuleRefused)
	{
		build({{"scripts/server/stategraphs/a.luau",
				"stategraph \"walker\" { initial = \"a\", states = { a = { lenght = 3 } } }"},
			   {"scripts/server/stategraphs/b.luau",
				"stategraph \"b\" { initial = \"a\", states = { a = { length = 3, next = \"zzz\" } } }"},
			   {"scripts/server/stategraphs/c.luau", "stategraph \"c\" { initial = \"a\", states = { a = { timeline = "
													 "{ [0] = \"go\" }, on = { og = function() end } } } }"},
			   {"scripts/server/stategraphs/d.luau", "stategraph \"d\" { states = { a = {} } }"},
			   {"scripts/client/stategraphs/e.luau", "stategraph \"e\" { initial = \"a\", states = { a = {} } }"},
			   {"scripts/server/stategraphs/f.luau",
				"stategraph \"f\" { initial = \"a\", states = { a = { length = 3 } } }"},
			   {"scripts/server/stategraphs/g.luau", "stategraph \"g\" { initial = \"a\", states = { a = { events = { "
													 "hurt = \"a\" }, react = { hurt = function() end } } } }"},
			   {"scripts/server/stategraphs/h.luau",
				"stategraph \"h\" { initial = \"a\", stage = \"Nowhere\", states = { a = {} } }"},
			   {"scripts/server/stategraphs/i.luau",
				"stategraph \"i\" { initial = \"a\", states = { a = {} }, extra = 1 }"}});

		EXPECT_NE(problem_with("unknown key 'lenght'; a state has every, enter, update, exit, timeline, marks, on, "
							   "length, next, events, react, ignore, show"),
				  nullptr);
		EXPECT_NE(problem_with("next names 'zzz', which is not a state"), nullptr);
		EXPECT_NE(problem_with("stategraph d: no initial state"), nullptr);
		EXPECT_NE(problem_with("a stategraph runs in the simulation, from scripts/sim or scripts/server"), nullptr);
		EXPECT_NE(problem_with("length without next"), nullptr);
		EXPECT_NE(problem_with("an event is answered twice"), nullptr);
		EXPECT_NE(problem_with("no stage named 'Nowhere'"), nullptr);
		EXPECT_NE(problem_with(
					  "unknown key 'extra'; a stategraph has initial, extends, stage, states, on, every, update, show"),
				  nullptr);

		// An on key that is no mark waits for an event of that name; one a letter or two from a mark is likely a slip.
		const Problem* slip =
			problem_with("on.og is no mark, so it waits for an event of that name; did you mean 'go'?");
		ASSERT_NE(slip, nullptr);
		EXPECT_EQ(slip->severity, Severity::Warning);
		EXPECT_EQ(m_host->stats().modules, 1u) << "only c, whose slip is a warning, loads";
		EXPECT_EQ(m_host->stats().stategraphs, 1u);
	}

	TEST_F(StategraphTest, AReloadKeepsTheStateAndTakesTheNewHandlers)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = {
					a = { enter = function(e) e.Scratch.value = 1 end, length = ticks(1), next = "b" },
					b = { update = function(e) e.Scratch.wide += 1 end },
				},
			}
		)"}});
		const ecs::Entity walker = m_world->spawn(WALKER);
		tick(2);
		EXPECT_EQ(m_host->state_of(walker), "b");
		EXPECT_EQ(scratch(walker).wide, 1.0);

		// The same states with another update: it carries on in b with the new one.
		load({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = {
					a = { enter = function(e) e.Scratch.value = 1 end, length = ticks(1), next = "b" },
					b = { update = function(e) e.Scratch.wide += 10 end },
				},
			}
		)"}});
		tick();
		EXPECT_EQ(m_host->state_of(walker), "b");
		EXPECT_EQ(scratch(walker).wide, 11.0);
		EXPECT_EQ(sg(walker).since, 2u) << "not re-entered";

		// A graph that lost the state it was in starts again from the initial one.
		load({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" { initial = "a", states = { a = { enter = function(e) e.Scratch.value = 9 end } } }
		)"}});
		tick();
		EXPECT_EQ(m_host->state_of(walker), "a");
		EXPECT_EQ(scratch(walker).value, 9u);
		EXPECT_TRUE(m_host->problems().empty()) << (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, DiceFollowTheNetworkIdSoTwoMachinesRollAlike)
	{
		// Two worlds whose walkers have different local numbers but one network id: the same rolls.
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = { a = { update = function(e) e.Scratch.wide = e.rng:range(1000000) + e.rng:range(1000) / 1000 end } },
			}
		)"}});
		ecs::World other(m_registry);
		physics::add_resources(other);
		Host& host = other.add_resource<Host>(other, HostDef{.budget = 20'000});
		bind(host.binding());
		host.reload(Span<const Source>(m_sources.data(), m_sources.size()));

		(void)other.spawn(PLAIN); // shifts the walker's local number in the other world
		const ecs::Entity mine	 = m_world->spawn(WALKER);
		const ecs::Entity theirs = other.spawn(WALKER);
		ASSERT_NE(entt::to_integral(mine), entt::to_integral(theirs));
		m_world->registry.emplace<net::NetId>(mine, net::NetId::make(40, 1));
		other.registry.emplace<net::NetId>(theirs, net::NetId::make(40, 1));

		for (u32 i = 0; i < 5; ++i)
		{
			tick();
			host.set_tick(m_tick);
			other.run(ecs::Phase::Simulate);
			EXPECT_EQ(scratch(mine).wide, other.registry.get<Scratch>(theirs).wide) << "tick " << m_tick;
		}
		EXPECT_NE(scratch(mine).wide, 0.0);
	}

	TEST_F(StategraphTest, AReloadThatAddsOrRemovesStatesLeavesEveryEntityWhereItWas)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = {
					a = { length = ticks(1), next = "m" },
					m = { update = function(e) e.Scratch.wide += 1 end },
				},
			}
		)"}});
		const ecs::Entity walker = m_world->spawn(WALKER);
		tick(2);
		ASSERT_EQ(m_host->state_of(walker), "m");

		// States before and after it in name order: it is still in m, and m's new update runs.
		load({{"scripts/server/stategraphs/walker.luau", R"(
			stategraph "walker" {
				initial = "a",
				states = {
					a = { length = ticks(1), next = "m" },
					b = { enter = function(e) e.Scratch.value = 99 end },
					m = { update = function(e) e.Scratch.wide += 100 end },
					z = {},
				},
			}
		)"}});
		tick();
		EXPECT_EQ(m_host->state_of(walker), "m");
		EXPECT_EQ(scratch(walker).wide, 101.0);
		EXPECT_EQ(scratch(walker).value, 0u) << "b was never entered";
		EXPECT_EQ(sg(walker).state, state_id("m"));
	}

	TEST_F(StategraphTest, TwoGraphsInTwoSlotsStepTogether)
	{
		build({{"scripts/sim/prefabs/twin.luau",
				"prefab \"twin\" { extends = \"plain\", Stategraph = { \"left\", \"right\" } }"},
			   {"scripts/server/stategraphs/left.luau", R"(
				stategraph "left" {
					initial = "a",
					states = {
						a = { update = function(e) e.Scratch.value += 1 end, events = { poke = "b" } },
						b = { enter = function(e) e.Scratch.flag = true end },
					},
				}
			)"},
			   {"scripts/server/stategraphs/right.luau", R"(
				stategraph "right" {
					initial = "x",
					states = {
						x = { update = function(e) e.Scratch.wide += 1 end, react = { poke = function(e) e.Scratch.wide += 100 end } },
					},
				}
			)"}});
		const ecs::Entity twin = m_world->spawn(*m_registry.prefab_types().find("twin"));
		tick();
		EXPECT_EQ(m_host->state_of(twin, 0), "a");
		EXPECT_EQ(m_host->state_of(twin, 1), "x");
		EXPECT_EQ(scratch(twin).value, 1u);
		EXPECT_EQ(scratch(twin).wide, 1.0);

		// One event, both graphs answer it.
		m_host->event(twin, "poke");
		tick();
		EXPECT_TRUE(scratch(twin).flag);
		EXPECT_EQ(m_host->state_of(twin, 0), "b");
		EXPECT_EQ(scratch(twin).wide, 102.0);
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

/** A frame drawn at these moments, in ticks: what a client's present() does before its Present systems. */
#define PRESENT_AT(predicted, interpolated)                                                                            \
	do                                                                                                                 \
	{                                                                                                                  \
		m_host->set_present(predicted, interpolated);                                                                  \
		m_world->run(ecs::Phase::Present);                                                                             \
	} while (false)

	TEST_F(StategraphTest, ShowsFireAsTheirMomentsAreDrawn)
	{
		// The graph's show parts write what they saw into Scratch: the state's entry, its mark, its exit, every
		// frame, and a cue the state raised, which the server forwards because a show names it.
		build({{"scripts/sim/prefabs/shown.luau", "prefab \"shown\" { extends = \"plain\", Stategraph = \"walker\" }"},
			   {"scripts/server/stategraphs/walker.luau", R"(
				stategraph "walker" {
					initial = "a",
					show = { hurt = function(e, cue) e.Scratch.wide += 1000 + cue.n end },
					states = {
						a = {
							enter = function(e) e:event("hurt", { n = 7 }) end,
							timeline = { [2] = "mid" },
							length = ticks(4),
							next = "b",
							show = {
								enter = function(e) e.Scratch.value += 1 end,
								mid = function(e) e.Scratch.value += 10 end,
								exit = function(e) e.Scratch.value += 100 end,
								update = function(e) e.Scratch.wide += 1 end,
							},
						},
						b = { show = { enter = function(e) e.Scratch.flag = true end } },
					},
				}
			)"}});
		const ecs::Entity shown = m_world->spawn(*m_registry.prefab_types().find("shown"));
		ASSERT_TRUE(m_world->registry.all_of<Cues>(shown)) << "a prefab with a graph carries cues";

		tick(); // enters a at tick 1, raises hurt at tick 1
		EXPECT_EQ(m_world->registry.get<Cues>(shown).count, 1u) << "the server forwarded the cue a show names";

		// Drawn before the state began: nothing yet.
		PRESENT_AT(-0.5, -0.5);
		EXPECT_EQ(scratch(shown).value, 0u);
		EXPECT_EQ(scratch(shown).wide, 0.0);

		// Drawn as tick 1 begins (moment 0): the entry, the frame, and the cue, once.
		PRESENT_AT(0.0, 0.0);
		EXPECT_EQ(scratch(shown).value, 1u);
		EXPECT_EQ(scratch(shown).wide, 1008.0);
		PRESENT_AT(0.5, 0.5);
		EXPECT_EQ(scratch(shown).value, 1u) << "entered once";
		EXPECT_EQ(scratch(shown).wide, 1009.0) << "the frame again, the cue not again";

		// The mark at 2 ticks in: drawn at moment 2.
		PRESENT_AT(1.9, 1.9);
		EXPECT_EQ(scratch(shown).value, 1u);
		PRESENT_AT(2.0, 2.0);
		EXPECT_EQ(scratch(shown).value, 11u);

		// The state ends at tick 5 (length 4 from tick 1): b is entered at tick 5, drawn from moment 4.
		tick(4);
		EXPECT_EQ(m_host->state_of(shown), "b");
		PRESENT_AT(3.5, 3.5);
		EXPECT_EQ(scratch(shown).value, 11u) << "b is not drawn yet";
		EXPECT_FALSE(scratch(shown).flag);
		PRESENT_AT(4.0, 4.0);
		EXPECT_EQ(scratch(shown).value, 111u) << "a's exit";
		EXPECT_TRUE(scratch(shown).flag) << "b's entry";
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, APrefabShowAndACueSeenLateAreHandled)
	{
		build({{"scripts/sim/prefabs/cued.luau", "prefab \"cued\" { extends = \"plain\", Cues = {} }"},
			   {"scripts/server/rules/raise.luau", R"(
				system("Act", function()
					for e, s in world:query(Scratch) do
						if now() == 2 then e:event("boom", { n = 3 }) end
					end
				end)
			)"},
			   {"scripts/client/shows/cued.luau", R"(
				show "cued" {
					enter = function(e) e.Scratch.value = 5 end,
					update = function(e) e.Scratch.wide += 1 end,
					boom = function(e, cue) e.Scratch.flag = cue.name == "boom" and cue.n == 3 and cue.late < 0.1 end,
				}
			)"}});
		const ecs::Entity cued = m_world->spawn(*m_registry.prefab_types().find("cued"));
		const ecs::Entity late = m_world->spawn(*m_registry.prefab_types().find("cued"));
		tick(2);
		EXPECT_EQ(m_world->registry.get<Cues>(cued).count, 1u);

		PRESENT_AT(1.0, 1.0);
		EXPECT_EQ(scratch(cued).value, 5u) << "entered at first sight";
		EXPECT_EQ(scratch(cued).wide, 1.0);
		EXPECT_TRUE(scratch(cued).flag) << "the cue, with its name and number";

		// One first drawn long after the cue: it is marked seen and never shown.
		m_world->registry.get<Scratch>(late).flag = false;
		m_world->registry.remove<Shown>(late);
		PRESENT_AT(100.0, 100.0);
		EXPECT_FALSE(scratch(late).flag);
		EXPECT_TRUE(m_world->registry.get<Shown>(late).has_seen(2, static_cast<u32>(hash_text("boom"))));
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, AClientShowsWhatItPredictsItselfOnce)
	{
		// A client world: its own player raises an event from a sim rule, which its show answers at the next
		// Present; the server's copy of the cue, arriving later, is one it has seen.
		build({{"scripts/sim/prefabs/own.luau", "prefab \"own\" { extends = \"plain\", Cues = {} }"},
			   {"scripts/sim/rules/swing.luau", R"(
				system("Act", function()
					for e, s in world:query(Scratch) do
						if now() == 3 then e:event("slash", { n = 1 }) end
					end
				end)
			)"},
			   {"scripts/client/shows/own.luau", R"(
				show "own" { slash = function(e, cue) e.Scratch.value += 1 end }
			)"}},
			  ecs::Role::Client);
		const ecs::Entity own = m_world->spawn(*m_registry.prefab_types().find("own"));
		m_world->registry.emplace<net::Owned>(own);
		m_world->registry.emplace<ecs::Simulated>(own);
		tick(3);
		EXPECT_EQ(m_world->registry.get<Cues>(own).count, 0u) << "a client writes no cues";

		PRESENT_AT(2.5, 0.0);
		EXPECT_EQ(scratch(own).value, 1u) << "shown from the client's own raise";

		// The server's word on it: the same cue, at the same tick, as replication would bring it.
		m_world->registry.get<Cues>(own).push(
			{.name = static_cast<u32>(hash_text("slash")), .tick = 3, .by = 0, .value = 1.0f});
		PRESENT_AT(3.5, 1.0);
		EXPECT_EQ(scratch(own).value, 1u) << "seen already";
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, AStoryReadsTopToBottomAndStartsOverOnReload)
	{
		build({{"scripts/sim/prefabs/teller.luau", "prefab \"teller\" { extends = \"plain\" }"},
			   {"scripts/server/stories/tale.luau", R"(
				story "tale" {
					prefab = "teller",
					run = function(e)
						e.Scratch.value = 1
						wait(ticks(2))
						e.Scratch.value = 2
						local who = wait_until(function() return if e.Scratch.flag then e else nil end, ticks(1))
						e.Scratch.value = if who == e then 3 else 99
						local event = wait_for("poke")
						e.Scratch.value = 4
						e.Scratch.wide = event.n
						wait(0)
						e.Scratch.value = 5
					end,
				}
			)"}});
		const ecs::Entity teller = m_world->spawn(*m_registry.prefab_types().find("teller"));
		EXPECT_EQ(m_host->stats().stories, 1u);

		tick(); // tick 1: runs to the first wait
		EXPECT_EQ(scratch(teller).value, 1u);
		EXPECT_EQ(m_host->stats().threads, 1u);
		tick(); // 2: still waiting (2 ticks from 1 wakes at 3)
		EXPECT_EQ(scratch(teller).value, 1u);
		tick(); // 3: on to the wait_until
		EXPECT_EQ(scratch(teller).value, 2u);
		tick(2);
		EXPECT_EQ(scratch(teller).value, 2u) << "the check answers nil";
		scratch(teller).flag = true;
		tick();
		EXPECT_EQ(scratch(teller).value, 3u) << "the check's answer is handed back";
		tick(3);
		EXPECT_EQ(scratch(teller).value, 3u) << "waiting for poke";
		m_host->event(teller, "poke", ecs::NO_ENTITY, 2.5f);
		tick();
		EXPECT_EQ(scratch(teller).value, 4u);
		EXPECT_EQ(scratch(teller).wide, 2.5);
		tick();
		EXPECT_EQ(scratch(teller).value, 5u) << "wait(0) is the next point";
		tick();
		EXPECT_EQ(m_host->stats().threads, 1u) << "done: at rest, not run again";
		EXPECT_EQ(scratch(teller).value, 5u);

		Vector<StoryReport> reports(&memory::heap(MemoryTag::Scripting));
		m_host->stories_of(teller, reports);
		ASSERT_EQ(reports.size(), 1u);
		EXPECT_EQ(reports[0].story, "tale");
		EXPECT_EQ(reports[0].waiting, "done");

		// The module saved: the story starts over from the top, on the new text.
		load({{"scripts/server/stories/tale.luau", R"(
				story "tale" { prefab = "teller", run = function(e) e.Scratch.value = 10 wait(ticks(1)) e.Scratch.value = 11 end }
			)"}});
		tick();
		EXPECT_EQ(scratch(teller).value, 10u);
		tick();
		EXPECT_EQ(scratch(teller).value, 11u);
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, StoriesReactLoopStartAndStop)
	{
		build({{"scripts/sim/prefabs/teller.luau", "prefab \"teller\" { extends = \"plain\" }"},
			   {"scripts/server/stories/tales.luau", R"(
				story "counter" {
					loop = true,
					run = function(e)
						e.Scratch.value += 1
						wait(ticks(2))
					end,
					on = {
						hurt = function(e, event)
							e.Scratch.wide = event.n
							return "restart"
						end,
					},
				}
				story "other" { run = function(e) e.Scratch.flag = true end }
				system("React", function()
					for e, s in world:query(Scratch) do
						if now() == 1 then e:start("counter") end
						if now() == 8 then e:stop("counter") end
						if now() == 9 then e:start("other") end
					end
				end)
			)"}});
		const ecs::Entity teller = m_world->spawn(*m_registry.prefab_types().find("teller"));
		tick(); // React at tick 1 starts it; its first step is the next Act point
		EXPECT_EQ(scratch(teller).value, 0u);
		tick(); // 2: runs once, waits 2
		EXPECT_EQ(scratch(teller).value, 1u);
		EXPECT_EQ(m_world->registry.get<Story>(teller).slots[0], static_cast<u32>(hash_text("counter")));
		tick(2); // 3: waiting; 4: wakes, finishes, and loops: it runs again at the next point
		EXPECT_EQ(scratch(teller).value, 1u);
		m_host->event(teller, "hurt", ecs::NO_ENTITY, 9.0f);
		tick(); // 5: the reaction restarts it, and it runs at once
		EXPECT_EQ(scratch(teller).wide, 9.0);
		EXPECT_EQ(scratch(teller).value, 2u);
		tick(3); // 6 waiting, 7 wakes and loops, 8 runs again (3), then React stops it
		EXPECT_EQ(scratch(teller).value, 3u);
		EXPECT_EQ(m_world->registry.get<Story>(teller).slots[0], 0u);
		tick(3);
		EXPECT_EQ(scratch(teller).value, 3u) << "stopped";
		EXPECT_TRUE(scratch(teller).flag) << "started on the other story";
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, SpawnHandsBackTheEntityAndAClientStoryWaitsInDrawnTicks)
	{
		build({{"scripts/sim/prefabs/teller.luau", "prefab \"teller\" { extends = \"plain\" }"},
			   {"scripts/server/rules/spawner.luau", R"(
				system("Act", function()
					if now() ~= 1 then return end
					local made: Entity? = nil
					for e, s in world:query(Scratch) do
						if made == nil then
							made = world:spawn("teller", { Scratch = { value = 7 } })
							made.Scratch.wide = 1.5
						end
						if e:id() == made:id() then s.flag = true end
					end
				end)
			)"},
			   {"scripts/client/stories/greet.luau", R"(
				story "greet" {
					prefab = "teller",
					run = function(e)
						local me = shown(e)
						me.Scratch.value = 100
						wait(ticks(2))
						me.Scratch.value = 101
					end,
				}
			)"}});
		const ecs::Entity first = m_world->spawn(*m_registry.prefab_types().find("teller"));
		tick();
		ecs::Entity made = ecs::NO_ENTITY;
		for (const auto [entity, s] : m_world->registry.view<const Scratch>().each())
			if (entity != first)
				made = entity;
		ASSERT_NE(made, ecs::NO_ENTITY);
		EXPECT_EQ(scratch(made).value, 7u);
		EXPECT_EQ(scratch(made).wide, 1.5);
		EXPECT_FALSE(scratch(made).flag) << "a query under way never visits what the point spawned";
		m_world->registry.destroy(first);

		// The client story runs at Present, in the drawn moment's ticks.
		PRESENT_AT(0.0, 0.0);
		EXPECT_EQ(scratch(made).value, 100u);
		PRESENT_AT(1.5, 1.5);
		EXPECT_EQ(scratch(made).value, 100u);
		PRESENT_AT(2.0, 2.0);
		EXPECT_EQ(scratch(made).value, 101u);
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, TheSecondFormNamesItsMarksMovesOnWithToAndAnswersInEveryState)
	{
		build(
			{{"scripts/sim/prefabs/second.luau", "prefab \"second\" { extends = \"plain\", Stategraph = \"second\" }"},
			 {"scripts/server/stategraphs/second.luau", R"(
				export type State = "idle" | "windup" | "strike" | "stunned"
				local g: Graph<State> = stategraph "second" {
					initial = "idle",
					on = { hurt = to "stunned" },
					update = function(e) e.Scratch.wide += 1 end,
				}
				g.state "idle" {
					update = function(e) return if e.Scratch.flag then "windup" else nil end,
				}
				-- Marks by name, a handler at one, and a length that is a mark.
				g.state "windup" {
					marks = { lift = 2, go = 4 },
					on = { lift = function(e) e.Scratch.value = e.state:at("go") * 10 + e.state:elapsed() end },
					length = "go",
					next = "strike",
				}
				-- Marks a function works out as the state is entered: here, from what the entity holds.
				g.state "strike" {
					marks = function(e) return { done = e.Scratch.value // 10 } end,
					length = "done",
					next = "idle",
					ignore = { "hurt" },
				}
				g.state "stunned" {
					length = 1,
					next = "idle",
					on = { hurt = function(e) e.Scratch.value = 999 end },
				}
				return g
			)"}});
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
		const ecs::Entity e = m_world->spawn(*m_registry.prefab_types().find("second"));

		tick(); // 1: idle, and the graph's update after the state's
		EXPECT_EQ(m_host->state_of(e), "idle");
		EXPECT_EQ(scratch(e).wide, 1.0);
		scratch(e).flag = true;
		tick(); // 2: idle -> windup
		EXPECT_EQ(m_host->state_of(e), "windup");
		EXPECT_EQ(scratch(e).wide, 2.0) << "once a tick, in whatever state it ends the tick in";
		tick(2); // 4: windup's lift, 2 ticks in
		EXPECT_EQ(scratch(e).value, 42u) << "at(\"go\") is 4; the mark fires 2 ticks in";
		tick(2); // 6: length = "go" (4): on into strike, whose marks come from Scratch.value // 10 = 4
		EXPECT_EQ(m_host->state_of(e), "strike");

		// A blow while it strikes: the graph answers hurt in every state, but strike ignores it.
		m_host->event(e, "hurt");
		tick();
		EXPECT_EQ(m_host->state_of(e), "strike");
		tick(3); // 10: done at 4 ticks in: back to idle, then windup again on the flag
		EXPECT_EQ(m_host->state_of(e), "windup");

		// In windup, a blow moves it on, through the graph's `to`; stunned answers its own.
		m_host->event(e, "hurt");
		tick();
		EXPECT_EQ(m_host->state_of(e), "stunned");
		m_host->event(e, "hurt");
		tick();
		EXPECT_EQ(scratch(e).value, 999u) << "the state's own on wins over the graph's";
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, AGraphExtendsAnotherStateByStateAndKeyByKey)
	{
		build({{"scripts/sim/prefabs/kin.luau", "prefab \"pink\" { extends = \"plain\", Stategraph = \"pink\" }"},
			   {"scripts/server/stategraphs/brown.luau", R"(
				local g = stategraph "brown" { initial = "a", on = { poke = to "c" } }
				g.state "a" { enter = function(e) e.Scratch.value = 1 end, length = 2, next = "b" }
				g.state "b" { enter = function(e) e.Scratch.value = 2 end }
				g.state "c" { enter = function(e) e.Scratch.value = 3 end }
				return g
			)"},
			   {"scripts/server/stategraphs/pink.luau", R"(
				local _ = require("./brown")
				local g = stategraph "pink" { extends = "brown" }
				-- a's enter stays brown's; where it goes changes. And a state brown has not.
				g.state "a" { length = 1, next = "flee" }
				g.state "flee" { enter = function(e) e.Scratch.value = 4 end }
				return g
			)"}});
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
		const ecs::Entity e = m_world->spawn(*m_registry.prefab_types().find("pink"));
		tick();
		EXPECT_EQ(scratch(e).value, 1u) << "brown's enter";
		tick();
		EXPECT_EQ(m_host->state_of(e), "flee") << "pink's length and next";
		EXPECT_EQ(scratch(e).value, 4u);
		m_host->event(e, "poke");
		tick();
		EXPECT_EQ(m_host->state_of(e), "c") << "brown's graph-wide on";
	}

	TEST_F(StategraphTest, AStrikeOpensTheHitboxUntilItsMarkItsStateOrItsFirstHit)
	{
		build({{"scripts/sim/stategraphs/biter.luau", R"(
				local g = stategraph "biter" { initial = "wait" }
				g.state "wait" { update = function(e) return if e.Scratch.flag then "bite" else nil end }
				g.state "bite" {
					marks = { open = 1, shut = 4 },
					on = { open = function(e) e:strike({ hits = 2, to = "shut", once = e.Scratch.value == 1 }) end },
					length = 6,
					next = "wait",
				}
				return g
			)"}});
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
		const ecs::Entity biter = m_world->spawn(BITER);
		const auto hits			= [&] { return m_world->registry.get<physics::Hitbox>(biter).hits.bits; };

		scratch(biter).flag = true;
		tick(); // 1: wait -> bite (since 1)
		EXPECT_EQ(hits(), 0u);
		scratch(biter).flag = false;
		tick(); // 2: open
		EXPECT_EQ(hits(), 2u);
		EXPECT_TRUE(m_world->registry.get<Strike>(biter).active);
		tick(2); // 4
		EXPECT_EQ(hits(), 2u);
		tick(); // 5: shut is 4 ticks in, since 1
		EXPECT_EQ(hits(), 0u) << "until its mark";

		// Once, against a target it touches: the hit at the strike's first collide shuts it at the next step.
		const ecs::Entity target								   = m_world->spawn(TARGET);
		m_world->registry.get<physics::Hurtbox>(target).layer.bits = 2;
		tick(2); // 7: bite's length is up: wait
		scratch(biter).value = 1;
		scratch(biter).flag	 = true;
		tick(); // 8: bite
		scratch(biter).flag = false;
		tick(); // 9: open; the hit lands at this tick's collide
		EXPECT_EQ(hits(), 2u);
		tick(); // 10: the step sees the hit
		EXPECT_EQ(hits(), 0u) << "off after its first hit";

		// A strike outlives no state: a push into another state shuts it.
		scratch(biter).value = 0;
		tick(4); // 14: back in wait
		scratch(biter).flag = true;
		tick(2); // bite, open
		EXPECT_EQ(hits(), 2u);
		ASSERT_TRUE(m_host->force_state(biter, 0, "wait"));
		scratch(biter).flag = false;
		tick();
		EXPECT_EQ(m_host->state_of(biter), "wait");
		EXPECT_EQ(hits(), 0u);
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, EachSlotSeesAnEventOnceAnsweredOrNot)
	{
		// poke is raised while the graph is in a, which does not answer it; a hands over to b, which would.
		build({{"scripts/server/stategraphs/walker.luau", R"(
				local g = stategraph "walker" { initial = "a" }
				g.state "a" { length = 2, next = "b" }
				g.state "b" { on = { poke = to "c" } }
				g.state "c" {}
				return g
			)"}});
		const ecs::Entity walker = m_world->spawn(WALKER);
		tick(); // 1: a
		m_host->event(walker, "poke");
		tick(); // 2: a sees poke and does not answer it
		tick(); // 3: b
		EXPECT_EQ(m_host->state_of(walker), "b");
		tick(2);
		EXPECT_EQ(m_host->state_of(walker), "b") << "an event a state let pass is not handed to the next";
	}

	TEST_F(StategraphTest, AnInspectorSeesSlotsMarksStoriesAndTheWatchedHistory)
	{
		build({{"scripts/server/stategraphs/walker.luau", R"(
				local g = stategraph "walker" { initial = "a" }
				g.state "a" { marks = { mid = 3 }, length = 5, next = "b" }
				g.state "b" {}
				return g
			)"}});
		const ecs::Entity walker = m_world->spawn(WALKER);
		m_host->watch(walker);
		tick(6); // a at 1, b at 6
		Inspection seen;
		m_host->inspect(walker, seen);
		ASSERT_TRUE(seen.found);
		ASSERT_EQ(seen.slots.size(), 1u);
		EXPECT_EQ(seen.slots[0].graph, "walker");
		EXPECT_EQ(seen.slots[0].state, "b");
		EXPECT_EQ(seen.slots[0].states.size(), 2u);
		ASSERT_EQ(seen.history.size(), 2u);
		EXPECT_EQ(seen.history[0].to, "a");
		EXPECT_EQ(seen.history[1].from, "a");
		EXPECT_EQ(seen.history[1].to, "b");
		EXPECT_EQ(seen.history[1].tick, 6u);

		ASSERT_TRUE(m_host->force_state(walker, 0, "a"));
		EXPECT_FALSE(m_host->force_state(walker, 0, "nowhere"));
		tick();
		m_host->inspect(walker, seen);
		EXPECT_EQ(seen.slots[0].state, "a");
		ASSERT_EQ(seen.slots[0].marks.size(), 1u);
		EXPECT_EQ(seen.slots[0].marks[0].name, "mid");
		EXPECT_EQ(seen.slots[0].marks[0].at, 3u);
		EXPECT_EQ(seen.history.back().to, "a");
	}

	TEST_F(StategraphTest, APrefabNamesItsGraph)
	{
		build({{"scripts/sim/prefabs/scripted.luau", "prefab \"scripted\" { extends = \"plain\", Stategraph = \"walker\" }\nprefab \"lost\" { extends = \"plain\", Stategraph = \"nowhere\" }"},
			   {"scripts/server/stategraphs/walker.luau", "stategraph \"walker\" { initial = \"a\", states = { a = { enter = function(e) e.Scratch.value = 4 end } } }"}});
		ASSERT_EQ(m_schema.problems.size(), 1u);
		EXPECT_NE(StringView(m_schema.problems[0].message).find("prefab lost names a stategraph no script declares"), StringView::npos);
		EXPECT_EQ(m_schema.problems[0].severity, Severity::Warning);

		const ecs::Prefab* scripted = m_registry.prefab_types().find("scripted");
		ASSERT_NE(scripted, nullptr);
		const ecs::Entity made = m_world->spawn(*scripted);
		EXPECT_EQ(sg(made).graph, graph_id("walker"));
		tick();
		EXPECT_EQ(m_host->state_of(made), "a");
		EXPECT_EQ(scratch(made).value, 4u);
	}
	TEST_F(StategraphTest, ModifiersStackCountDownTickAndChangeStats)
	{
		build({{"scripts/sim/modifiers/slowed.luau", R"(
				modifier "slowed" { lasts = ticks(10), stacking = "add", max_stacks = 2, stats = { move_speed = { mul = 0.5 } } }
				modifier "shielded" { stats = { armour = { add = 5 } } }
			)"},
			   {"scripts/server/modifiers/burning.luau", R"(
				modifier "burning" {
					lasts = 9,
					every = 3,
					power = 2,
					tick = function(e, m) e.Scratch.value += m.power e:event("burn") end,
					stats = { move_speed = { add = 1 } },
				}
				system("React", function()
					for e, s in world:query(Scratch) do
						if now() == 1 then e:inflict("burning") e:inflict("slowed") end
						if now() == 2 or now() == 3 then e:inflict("slowed") end
						if now() == 5 then
							local m = e:modifier("slowed")
							s.wide = if m then m.stacks * 100 + m.left else -1
							s.flag = e:stat("move_speed", 4) == 1.25 and e:modifier("shielded") == nil
						end
					end
				end)
			)"}});
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
		const ecs::Entity body = m_world->spawn(PLAIN);
		const u64 speed		   = stat_id("move_speed");
		EXPECT_EQ(m_host->stat(body, speed, 4.0f), 4.0f) << "no modifiers: the base itself";

		tick(); // 1: both inflicted at React
		const Modifiers& on = m_world->registry.get<Modifiers>(body);
		ASSERT_EQ(on.count, 2u);
		EXPECT_LT(on.list[0].name, on.list[1].name) << "sorted by name, so stats combine alike everywhere";
		EXPECT_EQ(m_host->stat(body, speed, 4.0f), 2.5f) << "(4 + 1) x 0.5";
		tick(2); // 2, 3: slowed again, stacking to its most
		EXPECT_EQ(m_host->stat(body, speed, 4.0f), 1.25f) << "(4 + 1) x 0.5 x 0.5: two stacks, never three";

		tick(2); // 5: the script reads it: two stacks, its clock restarted at 3 (until 13)
		EXPECT_EQ(scratch(body).wide, 208.0);
		EXPECT_TRUE(scratch(body).flag) << "e:stat agrees with the game's, and one it has not is nil";
		// burning: until 10, every 3 from its end: ticks at 4 and 7.
		EXPECT_EQ(scratch(body).value, 2u);
		tick(2); // 7
		EXPECT_EQ(scratch(body).value, 4u);
		tick(3); // 10: burning is gone at the first stage, before its tick could run
		EXPECT_EQ(scratch(body).value, 4u);
		EXPECT_EQ(m_world->registry.get<Modifiers>(body).count, 1u);
		EXPECT_EQ(m_host->stat(body, speed, 4.0f), 1.0f);
		tick(3); // 13: slowed is gone too
		EXPECT_EQ(m_world->registry.get<Modifiers>(body).count, 0u);
		EXPECT_EQ(m_host->stat(body, speed, 4.0f), 4.0f) << "the base again, to the bit";
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());

		Inspection seen;
		m_host->inspect(body, seen);
		EXPECT_TRUE(seen.modifiers.empty());
	}

	TEST_F(StategraphTest, AModifiersRightsAndMistakesAreNamed)
	{
		build({{"scripts/client/modifiers/wrong.luau", "modifier \"wrong\" { lasts = 3 }"},
			   {"scripts/server/modifiers/odd.luau", "modifier \"odd\" { lasts = 3, stacking = \"sometimes\" }"},
			   {"scripts/server/modifiers/fine.luau", R"(
				modifier "fine" { lasts = 30 }
				system("Act", function()
					for e, s in world:query(Scratch) do
						if now() == 1 then e:inflict("fine", { ticks = 5, power = 9 }) end
						if now() == 2 then e:inflict("nothing") end
					end
				end)
			)"},
			   {"scripts/client/rules/cure.luau", R"(
				system("Present", function()
					for e, s in world:query(Scratch) do e:cure() end
				end)
			)"}});
		EXPECT_NE(problem_with("declared in scripts/sim or scripts/server"), nullptr);
		EXPECT_NE(problem_with("stacking is \"refresh\", \"add\" or \"keep\""), nullptr);
		const ecs::Entity body = m_world->spawn(PLAIN);
		tick();
		const Modifiers& on = m_world->registry.get<Modifiers>(body);
		ASSERT_EQ(on.count, 1u);
		EXPECT_EQ(on.list[0].until, 6u);
		EXPECT_EQ(on.list[0].power, 9u);

		Inspection seen;
		m_host->inspect(body, seen);
		ASSERT_EQ(seen.modifiers.size(), 1u);
		EXPECT_EQ(seen.modifiers[0].name, "fine");
		EXPECT_EQ(seen.modifiers[0].left, 5u);

		tick();
		EXPECT_NE(problem_with("no modifier named 'nothing'"), nullptr);
		PRESENT_AT(1.0, 1.0);
		EXPECT_NE(problem_with("e:cure"), nullptr) << "a client cures nothing: the server's word stands";
		EXPECT_EQ(m_world->registry.get<Modifiers>(body).count, 1u);
	}

	TEST_F(StategraphTest, AModifiersShowEntersLeavesAndAnswersItsCues)
	{
		build(
			{{"scripts/sim/prefabs/burnt.luau", "prefab \"burnt\" { extends = \"plain\", Cues = {}, Modifiers = {} }"},
			 {"scripts/server/modifiers/burning.luau", R"(
				modifier "burning" {
					lasts = 6,
					every = 2,
					tick = function(e, m) e:event("burn", { n = m.power }) end,
					show = {
						enter = function(e) e.Scratch.value += 1 end,
						update = function(e) e.Scratch.wide += 1 end,
						exit = function(e) e.Scratch.value += 100 end,
						burn = function(e, cue) e.Scratch.value += 10 end,
					},
				}
				system("Act", function()
					for e, s in world:query(Scratch) do
						if now() == 1 then e:inflict("burning") end
					end
				end)
			)"}});
		const ecs::Entity burnt = m_world->spawn(*m_registry.prefab_types().find("burnt"));
		tick(); // 1: inflicted at Act, after the modifiers ran: until 7
		PRESENT_AT(0.5, 0.5);
		EXPECT_EQ(scratch(burnt).value, 1u) << "entered as it appears";
		EXPECT_EQ(scratch(burnt).wide, 1.0);

		tick(2); // 3: ticks at 3 (7 - 3 = 4), raising burn, which a show answers: a cue
		EXPECT_EQ(m_world->registry.get<Cues>(burnt).count, 1u);
		PRESENT_AT(2.5, 2.5);
		EXPECT_EQ(scratch(burnt).value, 11u) << "its show answers the cue its tick raised";
		tick(4); // 7: gone
		EXPECT_EQ(m_world->registry.get<Modifiers>(burnt).count, 0u);
		PRESENT_AT(6.5, 6.5);
		EXPECT_EQ(scratch(burnt).value, 121u) << "the 5's burn, then its exit";
		const f64 frames = scratch(burnt).wide;
		PRESENT_AT(7.5, 7.5);
		EXPECT_EQ(scratch(burnt).wide, frames) << "no frames once it has left";
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(StategraphTest, TriggersSayWhatEntersAndLeavesToAStoryOfReactions)
	{
		build({{"scripts/server/stories/pit.luau", R"(
				story "pit" {
					prefab = "pit",
					on = {
						entered = function(e, event)
							e.Scratch.value += 1
							if event.by then event.by.Scratch.flag = true end
						end,
						left = function(e, event) e.Scratch.value += 100 end,
					},
				}
			)"},
			   {"scripts/server/rules/who.luau", R"(
				system("Act", function()
					for e, s in world:query(Scratch) do
						if now() == 1 then s.wide = if e:prefab() == "pit" then 1 elseif e:prefab() == "body" then 2 else 3 end
					end
				end)
			)"}});
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
		const ecs::Entity pit						= m_world->spawn(PIT);
		const ecs::Entity body						= m_world->spawn(BODY);
		m_world->registry.get<Position>(body).value = {100.0f, 0.0f};

		tick(); // 1: apart
		EXPECT_EQ(scratch(pit).wide, 1.0);
		EXPECT_EQ(scratch(body).wide, 2.0);
		EXPECT_EQ(scratch(pit).value, 0u);

		m_world->registry.get<Position>(body).value = {6.0f, 0.0f};
		tick(); // 2: sensed at React, answered at the next point
		tick();
		EXPECT_EQ(scratch(pit).value, 1u);
		EXPECT_TRUE(scratch(body).flag) << "by is what came in";
		tick(3);
		EXPECT_EQ(scratch(pit).value, 1u) << "entered once while it stays";

		m_world->registry.get<Position>(body).value = {40.0f, 0.0f};
		tick(2);
		EXPECT_EQ(scratch(pit).value, 101u);

		Vector<StoryReport> reports(&memory::heap(MemoryTag::Scripting));
		m_host->stories_of(pit, reports);
		ASSERT_EQ(reports.size(), 1u);
		EXPECT_EQ(reports[0].waiting, "on") << "a story of reactions alone";
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}
}
