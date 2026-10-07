#include <ember/anim/components.h>
#include <ember/core/hash.h>
#include <ember/ecs/system.h>
#include <ember/net/serialize.h>
#include <ember/script/components.h>
#include <ember/script/host.h>
#include <ember/script/lua.h>
#include <ember/script/schema.h>

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

	[[nodiscard]] constexpr GraphId graph(StringView name) noexcept { return {static_cast<u32>(hash_text(name))}; }

	inline constexpr auto PLAIN	 = ecs::prefab("plain", Position{}, Scratch{}, anim::Playing{});
	inline constexpr auto WALKER = ecs::prefab("walker", Position{}, Scratch{}, anim::Playing{}, Stategraph{.graph = graph("walker")});
	inline constexpr auto SIM_WALKER = ecs::prefab("sim_walker", Position{}, Scratch{}, Stategraph{.graph = graph("sim_walker")});

	void act_scripts(Host& host, ecs::Commands& commands) { host.simulate(static_cast<u8>(Stage::Act), commands); }
	void react_scripts(Host& host, ecs::Commands& commands) { host.simulate(static_cast<u8>(Stage::React), commands); }

	void bind(Binding& binding)
	{
		binding.expose<Position>();
		binding.expose<Scratch>({.derived = true});
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
			m_registry.prefabs(PLAIN, WALKER, SIM_WALKER);
			register_components(m_registry);
			m_registry.simulate_exclusive<&act_scripts>(Stage::Act);
			m_registry.simulate_exclusive<&react_scripts>(Stage::React);
			m_registry.present_exclusive<&run_present>();

			for (const File& file : files)
				m_sources.push_back(make(file.first, file.second));
			m_schema = apply_schema(m_registry, Span<const Source>(m_sources.data(), m_sources.size()), &bind);

			m_world.emplace(m_registry, role);
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
		[[nodiscard]] const Stategraph& sg(ecs::Entity entity) { return m_world->registry.get<Stategraph>(entity); }

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
		EXPECT_EQ(sg(walker).state, Stategraph::NO_STATE);
		EXPECT_EQ(m_host->state_of(walker), "");

		tick();
		EXPECT_EQ(m_host->state_of(walker), "a");
		EXPECT_EQ(scratch(walker).value, 1u);
		EXPECT_EQ(sg(walker).since, 1u);
		EXPECT_EQ(sg(walker).state, 0u) << "states are numbered in name order";

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
		build({{"scripts/server/stategraphs/a.luau", "stategraph \"walker\" { initial = \"a\", states = { a = { lenght = 3 } } }"},
			   {"scripts/server/stategraphs/b.luau", "stategraph \"b\" { initial = \"a\", states = { a = { length = 3, next = \"zzz\" } } }"},
			   {"scripts/server/stategraphs/c.luau", "stategraph \"c\" { initial = \"a\", states = { a = { timeline = { [0] = \"go\" }, on = { nope = function() end } } } }"},
			   {"scripts/server/stategraphs/d.luau", "stategraph \"d\" { states = { a = {} } }"},
			   {"scripts/client/stategraphs/e.luau", "stategraph \"e\" { initial = \"a\", states = { a = {} } }"},
			   {"scripts/server/stategraphs/f.luau", "stategraph \"f\" { initial = \"a\", states = { a = { length = 3 } } }"},
			   {"scripts/server/stategraphs/g.luau", "stategraph \"g\" { initial = \"a\", states = { a = { events = { hurt = \"a\" }, react = { hurt = function() end } } } }"},
			   {"scripts/server/stategraphs/h.luau", "stategraph \"h\" { initial = \"a\", stage = \"Nowhere\", states = { a = {} } }"},
			   {"scripts/server/stategraphs/i.luau", "stategraph \"i\" { initial = \"a\", states = { a = {} }, extra = 1 }"}});

		EXPECT_NE(problem_with("unknown key 'lenght'; a state has every, enter, update, exit, timeline, on, length, next, events, react"), nullptr);
		EXPECT_NE(problem_with("next names 'zzz', which is not a state"), nullptr);
		EXPECT_NE(problem_with("on.nope names no mark of the timeline"), nullptr);
		EXPECT_NE(problem_with("stategraph d: no initial state"), nullptr);
		EXPECT_NE(problem_with("a stategraph runs in the simulation, from scripts/sim or scripts/server"), nullptr);
		EXPECT_NE(problem_with("length without next"), nullptr);
		EXPECT_NE(problem_with("an event is in both events and react"), nullptr);
		EXPECT_NE(problem_with("no stage named 'Nowhere'"), nullptr);
		EXPECT_NE(problem_with("unknown key 'extra'; a stategraph has initial, stage and states"), nullptr);
		EXPECT_EQ(m_host->stats().modules, 0u);
		EXPECT_EQ(m_host->stats().stategraphs, 0u);
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
		EXPECT_EQ(sg(made).graph, graph("walker"));
		tick();
		EXPECT_EQ(m_host->state_of(made), "a");
		EXPECT_EQ(scratch(made).value, 4u);
	}
}
