#include <ember/core/filesystem.h>
#include <ember/ecs/system.h>
#include <ember/jobs/job_system.h>
#include <ember/net/serialize.h>
#include <ember/physics/components.h>
#include <ember/script/dmath.h>
#include <ember/script/host.h>
#include <ember/script/lua.h>

#include <gtest/gtest.h>

#include <glm/vec2.hpp>

#include <initializer_list>
#include <string>
#include <utility>

#if defined(EMBER_PLATFORM_WINDOWS)
	#include <process.h>
#else
	#include <unistd.h>
#endif

/**
 * A small game of its own: a few components of every kind, two prefabs, two stages and a Present
 * point, with scripts given as text. What the sword will do, in miniature: queries, reads and writes
 * with their rights, verbs, require, reload, errors, the budget and the definitions.
 */
namespace host_test
{
	using namespace ember;
	using namespace ember::script;

	struct Position
	{
		glm::vec2 value = {};
		bool jumped		= false;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_float(stream, value.x);
			serialize_float(stream, value.y);
			serialize_bool(stream, jumped);
			return true;
		}
	};
	EMBER_COMPONENT(Position, Interpolated | Predicted);

	struct Weapon
	{
		i8 swipe	= 1;
		u8 cooldown = 0;
		u32 swung	= 0;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_int(stream, swipe, -1, 1);
			serialize_int(stream, cooldown, 0, 255);
			serialize_bits(stream, swung, 32);
			return true;
		}
	};
	EMBER_COMPONENT(Weapon, Predicted);

	struct Health
	{
		i16 current = 5;
		i16 most	= 5;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_int(stream, current, -1000, 1000);
			serialize_int(stream, most, 0, 1000);
			return true;
		}
	};
	EMBER_COMPONENT(Health, Replicated);

	struct Brain
	{
		u8 state = 0;
	};
	EMBER_COMPONENT(Brain, Server);

	struct Glow
	{
		f32 amount = 0.0f;
	};
	EMBER_COMPONENT(Glow, Client);

	struct Tagged
	{
	};
	EMBER_COMPONENT(Tagged, Sim);

	/** Where scripts leave answers for the tests: derived, so every context may write it. */
	struct Scratch
	{
		u32 value	= 0;
		f64 wide	= 0.0;
		bool flag	= false;
		glm::vec2 point = {};
		i8 small	= 0;
	};
	EMBER_COMPONENT(Scratch, Sim);

	enum class Layer : u32
	{
		None   = 0,
		Player = 1 << 1,
		Enemy  = 1 << 2,
	};

	enum class Stage : u8
	{
		Act,
		React,
		Count
	};

	inline constexpr auto DUMMY = ecs::prefab("dummy", Position{}, Weapon{}, Health{}, physics::Hitbox{}, Scratch{});
	inline constexpr auto GHOST = ecs::prefab("ghost", Position{}, Scratch{}, Tagged{});

	void act_scripts(Host& host, ecs::Commands& commands) { host.simulate(static_cast<u8>(Stage::Act), commands); }
	void react_scripts(Host& host, ecs::Commands& commands) { host.simulate(static_cast<u8>(Stage::React), commands); }

	/** A C++ system beside the script point, for the schedule tests. */
	void count_weapons(ecs::Sim<const Weapon> weapons) { (void)weapons; }

	int weapon_ready(lua_State* L, Weapon& weapon, ecs::Entity)
	{
		lua_pushboolean(L, weapon.cooldown == 0);
		return 1;
	}

	/** A game function on every entity: the distance to another. */
	int entity_distance(lua_State* L)
	{
		ecs::World& world = world_of(L);
		const ecs::Entity a = check_entity(L, 1);
		const ecs::Entity b = check_entity(L, 2);
		const glm::vec2 d	= world.registry.get<Position>(a).value - world.registry.get<Position>(b).value;
		lua_pushnumber(L, static_cast<f64>(d.x * d.x + d.y * d.y));
		return 1;
	}

	/** A server-only library function. */
	int fx_mark(lua_State* L)
	{
		world_of(L).registry.get<Scratch>(check_entity(L, 1)).value = 99;
		return 0;
	}

	constexpr Named<Layer> LAYERS[]	  = {{"None", Layer::None}, {"Player", Layer::Player}, {"Enemy", Layer::Enemy}};
	constexpr Function FX[]			  = {{"mark", "(entity: Entity) -> ()", fx_mark, ContextMask::Server}};
	constexpr Function DISTANCE		  = {"distance", "(other: Entity) -> number", entity_distance, ContextMask::All};

	void bind(Binding& binding)
	{
		binding.expose<Position>();
		binding.expose<Weapon>();
		binding.expose<Health>();
		binding.expose<Brain>();
		binding.expose<Glow>();
		binding.expose<Tagged>();
		binding.expose<Scratch>({.derived = true});
		binding.expose<physics::Hitbox>({.derived = true});
		binding.method<Weapon, &weapon_ready>("ready", "() -> boolean");
		binding.stage("Act", static_cast<u8>(Stage::Act));
		binding.stage("React", static_cast<u8>(Stage::React));
		binding.constant("TILE", 16.0);
		binding.enumeration<Layer>("Layer", LAYERS);
		binding.library("fx", FX);
		binding.entity_method(DISTANCE);
	}

	struct Game
	{
		ecs::Registry registry;

		Game()
		{
			registry.prefabs(DUMMY, GHOST);
			registry.components<Brain, Glow>();
			registry.simulate_exclusive<&act_scripts>(Stage::Act);
			registry.simulate<count_weapons>(Stage::Act);
			registry.simulate_exclusive<&react_scripts>(Stage::React);
			registry.present_exclusive<&run_present>();
		}
	};

	using File = std::pair<const char*, const char*>;

	class HostTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			auto parsed = Aliases::parse("scripts", bytes_of(R"({ "aliases": { "lib": "./lib" } })"));
			ASSERT_TRUE(parsed.has_value());
			m_aliases = std::move(*parsed);

			m_host = &m_world.add_resource<Host>(m_world, HostDef{.budget = 20'000});
			bind(m_host->binding());
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
				m_world.run(ecs::Phase::Simulate);
			}
		}

		void present() { m_world.run(ecs::Phase::Present); }

		[[nodiscard]] const Problem* problem_with(StringView fragment) const
		{
			for (const Problem& problem : m_host->problems())
				if (StringView(problem.message).find(fragment) != StringView::npos)
					return &problem;
			return nullptr;
		}

		[[nodiscard]] Scratch& scratch(ecs::Entity entity) { return m_world.registry.get<Scratch>(entity); }

		Game m_game;
		ecs::World m_world{m_game.registry, ecs::Role::Standalone};
		Host* m_host = nullptr;
		Aliases m_aliases;
		u32 m_tick = 0;
	};

	TEST_F(HostTest, ASimSystemReadsAndWritesPredictedFields)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		load({{"scripts/sim/a.luau", "system('Act', function()\n"
									 "  for e, w in world:query(Weapon) do\n"
									 "    w.cooldown = w.cooldown + 1\n"
									 "    w.swipe = -w.swipe\n"
									 "    w.swung = now()\n"
									 "  end\n"
									 "end)"}});
		tick(3);

		const Weapon& weapon = m_world.registry.get<Weapon>(dummy);
		EXPECT_EQ(weapon.cooldown, 3);
		EXPECT_EQ(weapon.swipe, -1);
		EXPECT_EQ(weapon.swung, 3u);
		EXPECT_TRUE(m_host->problems().empty());

		const Stats stats = m_host->stats();
		EXPECT_EQ(stats.calls, 3u);
		EXPECT_EQ(stats.queries, 3u);
		EXPECT_EQ(stats.modules, 1u);
		EXPECT_EQ(stats.systems, 1u);
		EXPECT_EQ(stats.errors, 0u);
		EXPECT_NE(stats.hash, 0u);
	}

	TEST_F(HostTest, ASimWriteOfReplicatedStateIsRefusedAndTheSystemGoesQuiet)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		load({{"scripts/sim/a.luau", "system('Act', function()\n"
									 "  for e, h in world:query(Health) do\n"
									 "    h.current = 1\n"
									 "  end\n"
									 "end)"}});
		tick(5);

		EXPECT_EQ(m_world.registry.get<Health>(dummy).current, 5);
		ASSERT_EQ(m_host->problems().size(), 1u);
		const Problem& problem = m_host->problems()[0];
		EXPECT_EQ(problem.path, "scripts/sim/a.luau");
		EXPECT_EQ(problem.line, 3u);
		EXPECT_EQ(problem.count, 1u); // off after the first, not raised every tick
		EXPECT_NE(StringView(problem.message).find("Predicted"), StringView::npos);
		EXPECT_EQ(m_host->stats().disabled, 1u);

		// A reload of the module turns it back on, and the same mistake counts again.
		load({{"scripts/sim/a.luau", "system('Act', function()\n"
									 "  for e, h in world:query(Health) do\n"
									 "    h.current = 1\n"
									 "  end\n"
									 "end)"}});
		tick(1);
		ASSERT_EQ(m_host->problems().size(), 1u);
		EXPECT_EQ(m_host->problems()[0].count, 1u); // its problems were cleared by the reload, and this is the new one
		EXPECT_EQ(m_host->stats().errors, 2u);
	}

	TEST_F(HostTest, QueriesFilterAndFollowWhatTheWorldSimulates)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		const ecs::Entity ghost = m_world.spawn(GHOST);
		const ecs::Entity still = m_world.create(m_world.prefab(GHOST), false); // not simulated

		load({{"scripts/sim/a.luau", "system('Act', function()\n"
									 "  local all, untagged, ghosts = 0, 0, 0\n"
									 "  for e, s in world:query(Scratch) do all += 1 end\n"
									 "  for e, s in world:query(Scratch, without(Tagged)) do untagged += 1 end\n"
									 "  for e, s, t in world:query(Scratch, Tagged) do ghosts += 1 end\n"
									 "  for e, s in world:query(Scratch) do s.value = all * 100 + untagged * 10 + ghosts end\n"
									 "end)"}});
		tick();

		EXPECT_EQ(scratch(dummy).value, 211u);
		EXPECT_EQ(scratch(ghost).value, 211u);
		EXPECT_EQ(scratch(still).value, 0u);
		EXPECT_TRUE(m_host->problems().empty());
	}

	TEST_F(HostTest, AbsentComponentsAreNilAndHasAnswers)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		load({{"scripts/sim/a.luau", "system('Act', function()\n"
									 "  for e, s in world:query(Scratch) do\n"
									 "    s.flag = e.Brain == nil and e.Weapon ~= nil and e:has(Weapon) and not e:has(Tagged)\n"
									 "    s.value = e.Weapon.cooldown + 7\n"
									 "  end\n"
									 "end)"}});
		tick();
		EXPECT_TRUE(scratch(dummy).flag);
		EXPECT_EQ(scratch(dummy).value, 7u);
	}

	TEST_F(HostTest, ClientScriptsPresentEverythingAndWriteClientComponentsOnly)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		const ecs::Entity still = m_world.create(m_world.prefab(GHOST), false);
		m_world.registry.emplace<Glow>(dummy);
		m_world.registry.emplace<Glow>(still);

		load({{"scripts/client/a.luau", "system('Present', function()\n"
										"  for e, g in world:query(Glow) do g.amount = g.amount + 0.5 end\n"
										"  for e, p in world:query(Position) do p.jumped = true end\n"
										"end)"}});
		present();
		present();

		EXPECT_FLOAT_EQ(m_world.registry.get<Glow>(dummy).amount, 0.5f); // the second frame hit the refusal first
		EXPECT_FLOAT_EQ(m_world.registry.get<Glow>(still).amount, 0.5f);
		EXPECT_FALSE(m_world.registry.get<Position>(dummy).jumped);
		ASSERT_NE(problem_with("client script"), nullptr);
		EXPECT_EQ(problem_with("client script")->line, 3u);
		EXPECT_EQ(m_host->stats().present_us >= 0.0, true);
	}

	TEST_F(HostTest, ServerScriptsSpawnAddRemoveAndDestroy)
	{
		m_world.spawn(DUMMY);
		const ecs::Entity ghost = m_world.spawn(GHOST);

		load({{"scripts/server/a.luau",
			   "system('Act', function()\n"
			   "  local dummies = 0\n"
			   "  for e, w in world:query(Weapon) do dummies += 1 end\n"
			   "  if dummies == 1 then\n"
			   "    world:spawn('dummy', { Position = { value = vector.create(3, 4, 0) }, Weapon = { cooldown = 7 } })\n"
			   "  end\n"
			   "  for e, t in world:query(Tagged) do\n"
			   "    if e:has(Brain) then e:destroy() else e:add(Brain, { state = 2 }) end\n"
			   "  end\n"
			   "  for e, h in world:query(Health) do h.current = h.current - 1 end\n"
			   "end)"}});
		tick();

		// The spawn and the add landed when the stage ended.
		u32 dummies = 0, spawned = 0;
		for (auto [entity, position, weapon] : m_world.registry.view<const Position, const Weapon>().each())
		{
			++dummies;
			if (position.value == glm::vec2(3.0f, 4.0f) && weapon.cooldown == 7)
				++spawned;
		}
		EXPECT_EQ(dummies, 2u);
		EXPECT_EQ(spawned, 1u);
		ASSERT_TRUE(m_world.registry.all_of<Brain>(ghost));
		EXPECT_EQ(m_world.registry.get<Brain>(ghost).state, 2);

		tick();
		EXPECT_FALSE(m_world.registry.valid(ghost)); // destroyed on the second tick
		EXPECT_TRUE(m_host->problems().empty()) << m_host->problems()[0].message;
	}

	TEST_F(HostTest, SimScriptsMayNotSpawnOrDestroyButMayAddWhatTheyMayWrite)
	{
		const ecs::Entity ghost = m_world.spawn(GHOST);
		load({{"scripts/sim/a.luau", "system('Act', function()\n"
									 "  for e, t in world:query(Tagged) do\n"
									 "    if not e:has(Weapon) then e:add(Weapon, { cooldown = 3 }) else e:remove(Weapon) end\n"
									 "  end\n"
									 "end)"},
			  {"scripts/sim/b.luau", "system('React', function()\n"
									 "  for e, t in world:query(Tagged) do e:destroy() end\n"
									 "end)"}});
		tick();
		ASSERT_TRUE(m_world.registry.all_of<Weapon>(ghost));
		EXPECT_EQ(m_world.registry.get<Weapon>(ghost).cooldown, 3);
		EXPECT_TRUE(m_world.registry.valid(ghost));
		ASSERT_NE(problem_with("destroy"), nullptr);
		EXPECT_EQ(problem_with("destroy")->path, "scripts/sim/b.luau");

		tick();
		EXPECT_FALSE(m_world.registry.all_of<Weapon>(ghost));
	}

	TEST_F(HostTest, RequireLoadsLibsOnDemandAndAReloadKeepsTheLastGoodVersion)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);

		// The sim first, the lib after: require() finds it in the batch and loads it then and there.
		load({{"scripts/sim/a.luau", "local m = require('@lib/m')\n"
									 "system('Act', function() for e, s in world:query(Scratch) do s.value = m.k end end)"},
			  {"scripts/lib/m.luau", "return { k = 2 }"}});
		tick();
		EXPECT_EQ(scratch(dummy).value, 2u);
		EXPECT_EQ(m_host->stats().modules, 2u);

		// A new lib: the pair comes again, the lib first, and the sim sees the new value.
		load({{"scripts/lib/m.luau", "return { k = 3 }"},
			  {"scripts/sim/a.luau", "local m = require('@lib/m')\n"
									 "system('Act', function() for e, s in world:query(Scratch) do s.value = m.k end end)"}});
		tick();
		EXPECT_EQ(scratch(dummy).value, 3u);

		// A lib that fails at its top level stays as it was; the sim that reloads with it gets the old one.
		const u64 before = m_host->hash();
		load({{"scripts/lib/m.luau", "error('boom')"},
			  {"scripts/sim/a.luau", "local m = require('@lib/m')\n"
									 "system('Act', function() for e, s in world:query(Scratch) do s.value = m.k + 10 end end)"}});
		tick();
		EXPECT_EQ(scratch(dummy).value, 13u);
		ASSERT_NE(problem_with("boom"), nullptr);
		EXPECT_EQ(problem_with("boom")->path, "scripts/lib/m.luau");
		EXPECT_EQ(problem_with("boom")->line, 1u);
		EXPECT_NE(m_host->hash(), before); // the sim's text changed
		EXPECT_EQ(m_host->stats().modules, 2u);
	}

	TEST_F(HostTest, ARequireOfAModuleThatIsNotThereIsAnErrorAtLoad)
	{
		load({{"scripts/sim/a.luau", "local m = require('@lib/missing')\nsystem('Act', function() end)"}});
		EXPECT_EQ(m_host->stats().modules, 0u);
		EXPECT_NE(problem_with("did not load"), nullptr);
	}

	TEST_F(HostTest, LibsRunWithTheirCallersRights)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		load({{"scripts/lib/hurt.luau", "return { hurt = function(e) e.Health.current = 1 end }"},
			  {"scripts/sim/a.luau", "local h = require('@lib/hurt')\n"
									 "system('Act', function() for e, w in world:query(Weapon) do h.hurt(e) end end)"},
			  {"scripts/server/b.luau", "local h = require('@lib/hurt')\n"
										"system('React', function() for e, w in world:query(Weapon) do h.hurt(e) end end)"}});
		tick();
		EXPECT_EQ(m_world.registry.get<Health>(dummy).current, 1); // the server's call
		ASSERT_NE(problem_with("Predicted"), nullptr);			  // the sim's was refused, inside the lib
		EXPECT_EQ(problem_with("Predicted")->path, "scripts/lib/hurt.luau");
	}

	TEST_F(HostTest, ErrorsAreOneRecordAndTheBudgetStopsAnEndlessLoop)
	{
		m_world.spawn(DUMMY);
		load({{"scripts/sim/a.luau", "system('Act', function() error('bad') end)"},
			  {"scripts/sim/b.luau", "system('Act', function() while true do end end)"}});
		tick(3);

		ASSERT_EQ(m_host->problems().size(), 2u);
		EXPECT_NE(problem_with("bad"), nullptr);
		EXPECT_NE(problem_with("safepoints"), nullptr);
		EXPECT_EQ(m_host->stats().disabled, 2u);
		EXPECT_EQ(m_host->stats().errors, 2u);

		m_host->clear_problems();
		EXPECT_TRUE(m_host->problems().empty());
	}

	TEST_F(HostTest, FieldsAreCheckedForTypeAndRange)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		load({{"scripts/sim/a.luau", "system('Act', function() for e, w in world:query(Weapon) do w.cooldown = 300 end end)"},
			  {"scripts/sim/b.luau", "system('Act', function() for e, s in world:query(Scratch) do s.small = -129 end end)"},
			  {"scripts/sim/c.luau", "system('Act', function() for e, s in world:query(Scratch) do s.flag = 1 end end)"},
			  {"scripts/sim/d.luau", "system('Act', function() for e, s in world:query(Scratch) do s.point = 3 end end)"},
			  {"scripts/sim/e.luau", "system('Act', function() for e, s in world:query(Scratch) do s.nothing = 3 end end)"},
			  {"scripts/sim/f.luau", "system('Act', function() for e, s in world:query(Scratch) do s.point = vector.create(1, 2, 0) s.small = -128 s.wide = 2.5 end end)"}});
		tick();

		EXPECT_EQ(m_world.registry.get<Weapon>(dummy).cooldown, 0);
		EXPECT_NE(problem_with("0 to 255"), nullptr);
		EXPECT_NE(problem_with("-128 to 127"), nullptr);
		EXPECT_NE(problem_with("true or false"), nullptr);
		EXPECT_NE(problem_with("takes a vector"), nullptr);
		EXPECT_NE(problem_with("no field 'nothing'"), nullptr);
		EXPECT_EQ(scratch(dummy).point, glm::vec2(1.0f, 2.0f));
		EXPECT_EQ(scratch(dummy).small, -128);
		EXPECT_EQ(scratch(dummy).wide, 2.5);
	}

	TEST_F(HostTest, MethodsLibrariesEntityMethodsAndConstants)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		const ecs::Entity ghost = m_world.spawn(GHOST, Position{.value = {3.0f, 4.0f}});
		(void)ghost;

		load({{"scripts/sim/a.luau", "system('Act', function()\n"
									 "  for e, w, s in world:query(Weapon, Scratch) do\n"
									 "    s.flag = w:ready() and TILE == 16 and Layer.Enemy == 4\n"
									 "    for o, t in world:query(Tagged) do s.wide = e:distance(o) end\n"
									 "    s.small = typeof(e) == 'Entity' and typeof(w) == 'Component' and typeof(Weapon) == 'ComponentType' and tostring(Weapon) == 'Component Weapon' and 1 or 0\n"
									 "    fx.mark(e)\n"
									 "  end\n"
									 "end)"},
			  {"scripts/server/b.luau", "system('React', function() for e, w in world:query(Weapon) do fx.mark(e) end end)"}});
		tick();

		EXPECT_TRUE(scratch(dummy).flag);
		EXPECT_EQ(scratch(dummy).wide, 25.0);
		EXPECT_EQ(scratch(dummy).small, 1);
		EXPECT_EQ(scratch(dummy).value, 99u);					  // the server's fx.mark
		ASSERT_NE(problem_with("fx.mark: not from"), nullptr); // the sim's was refused
		EXPECT_EQ(problem_with("fx.mark: not from")->path, "scripts/sim/a.luau");
		EXPECT_EQ(problem_with("fx.mark: not from")->line, 6u);
	}

	TEST_F(HostTest, TheSandboxIsDeterministicWhereItMustBe)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		load({{"scripts/sim/a.luau", "system('Act', function()\n"
									 "  for e, s in world:query(Scratch) do\n"
									 "    s.wide = math.sin(1) + math.cos(2) * 1000 + math.atan2(1, 2) * 1e6 + math.exp(0.5) * 1e9\n"
									 "    s.flag = _G.math.random == nil and _G.coroutine == nil and _G.loadstring == nil and _G.getfenv == nil and _G.os == nil and _G.io == nil and _G.debug == nil\n"
									 "    s.value = #tostring(math.pow(2, 10))\n"
									 "  end\n"
									 "end)"},
			  {"scripts/server/b.luau", "system('React', function()\n"
										"  for e, s in world:query(Scratch) do s.small = (math.random ~= nil and _G.coroutine == nil and _G.math.random == nil) and 1 or 0 end\n"
										"end)"},
			  {"scripts/client/c.luau", "system('Present', function()\n"
										"  for e, s in world:query(Scratch) do s.point = vector.create(coroutine ~= nil and 1 or 0, math.random ~= nil and 1 or 0, 0) end\n"
										"end)"}});
		tick();
		present();

		const f64 expected = dmath::sin(1.0) + dmath::cos(2.0) * 1000.0 + dmath::atan2(1.0, 2.0) * 1e6 + dmath::exp(0.5) * 1e9;
		EXPECT_EQ(scratch(dummy).wide, expected);
		EXPECT_TRUE(scratch(dummy).flag);
		EXPECT_EQ(scratch(dummy).value, 4u); // "1024"
		EXPECT_EQ(scratch(dummy).small, 1);
		EXPECT_EQ(scratch(dummy).point, glm::vec2(1.0f, 1.0f));
		EXPECT_TRUE(m_host->problems().empty()) << m_host->problems()[0].message;
	}

	TEST_F(HostTest, SystemsRunInPathOrderWhateverOrderTheyLoadedIn)
	{
		const ecs::Entity dummy = m_world.spawn(DUMMY);
		load({{"scripts/sim/z.luau", "system('Act', function() for e, s in world:query(Scratch) do s.value = s.value * 10 + 3 end end)"},
			  {"scripts/sim/a.luau", "system('Act', function() for e, s in world:query(Scratch) do s.value = s.value * 10 + 1 end end)\n"
									 "system('Act', function() for e, s in world:query(Scratch) do s.value = s.value * 10 + 2 end end)"}});
		tick();
		EXPECT_EQ(scratch(dummy).value, 123u);

		// The same sources in another host, given the other way round: the same hash.
		Game other_game;
		ecs::World other_world(other_game.registry, ecs::Role::Standalone);
		Host& other = other_world.add_resource<Host>(other_world);
		bind(other.binding());
		Vector<Source> reversed(&memory::heap(MemoryTag::Scripting));
		reversed.push_back(make("scripts/sim/a.luau", "system('Act', function() for e, s in world:query(Scratch) do s.value = s.value * 10 + 1 end end)\n"
													  "system('Act', function() for e, s in world:query(Scratch) do s.value = s.value * 10 + 2 end end)"));
		reversed.push_back(make("scripts/sim/z.luau", "system('Act', function() for e, s in world:query(Scratch) do s.value = s.value * 10 + 3 end end)"));
		other.reload(reversed);
		EXPECT_EQ(other.hash(), m_host->hash());
	}

	TEST_F(HostTest, SystemsOutsideTheirContextAreRefusedAtLoad)
	{
		load({{"scripts/sim/a.luau", "system('Present', function() end)"},
			  {"scripts/client/b.luau", "system('Act', function() end)"},
			  {"scripts/lib/c.luau", "system('Act', function() end)"},
			  {"scripts/sim/d.luau", "system('Nowhere', function() end)"},
			  {"scripts/sim/e.luau", "local function later() system('Act', function() end) end\nsystem('Act', later)"}});
		EXPECT_NE(problem_with("Present is for scripts/client"), nullptr);
		EXPECT_NE(problem_with("a client script presents"), nullptr);
		EXPECT_NE(problem_with("a lib declares no systems"), nullptr);
		EXPECT_NE(problem_with("no stage named 'Nowhere'"), nullptr);
		EXPECT_EQ(m_host->stats().modules, 1u);

		tick();
		EXPECT_NE(problem_with("top level"), nullptr);
	}

	TEST_F(HostTest, TheWorldIsShutWhileAModuleLoads)
	{
		m_world.spawn(DUMMY);
		load({{"scripts/sim/a.luau", "for e, w in world:query(Weapon) do w.cooldown = 1 end\nsystem('Act', function() end)"}});
		EXPECT_NE(problem_with("not while a module loads"), nullptr);
		EXPECT_EQ(m_host->stats().modules, 0u);
	}

	TEST_F(HostTest, TheScriptPointRunsAloneInEveryMode)
	{
		const String schedule = m_world.describe(ecs::Phase::Simulate);
		EXPECT_NE(schedule.find("act_scripts"), String::npos);
		EXPECT_NE(schedule.find("+exclusive"), String::npos);

		const ecs::Entity dummy = m_world.spawn(DUMMY);
		load({{"scripts/sim/a.luau", "system('Act', function() for e, w in world:query(Weapon) do w.cooldown = w.cooldown + 1 end end)"}});

		jobs::initialize({.worker_count = 2});
		m_world.set_mode(ecs::RunMode::Jobs);
		tick(10);
		m_world.set_mode(ecs::RunMode::Shuffled, 7);
		tick(10);
		m_world.set_mode(ecs::RunMode::Serial);
		jobs::shutdown();

		EXPECT_EQ(m_world.registry.get<Weapon>(dummy).cooldown, 20);
		EXPECT_TRUE(m_host->problems().empty());
	}

	TEST_F(HostTest, DefinitionsDescribeTheBinding)
	{
		String path(&memory::heap(MemoryTag::Engine));
		ASSERT_TRUE(fs::temporary_directory(path).has_value());
		ASSERT_TRUE(fs::join(path, path, "ember_host_tests_" + std::to_string(getpid()) + ".d.luau").has_value());

		ASSERT_TRUE(m_host->write_definitions(path));
		const auto read = fs::read_file(path, memory::heap(MemoryTag::Engine));
		(void)fs::remove_file(path);
		ASSERT_TRUE(read.has_value());
		const StringView text(reinterpret_cast<const char*>(read->data()), read->size());

		for (const char* expected : {"declare extern type Weapon with", "\tcooldown: number", "\tfunction ready(self): boolean",
									 "declare Weapon: Component<Weapon>", "declare extern type Entity with", "\tWeapon: Weapon?",
									 "\tfunction distance(self, other: Entity): number", "\tfunction has(self, component: Component<any>): boolean",
									 "declare extern type Hitbox with", "\tshape: Shape", "\thits: number", "\tpoint: vector", "\twide: number",
									 "\"Act\" | \"React\" | \"Present\"", "declare fx: {", "\tmark: (entity: Entity) -> (),",
									 "declare Layer: {", "\tEnemy: number,", "declare TILE: number", "export type World = {",
									 "(<A, B>(self: World, a: Component<A>, b: Component<B>, ...Filter) -> () -> (Entity, A, B))"})
			EXPECT_NE(text.find(expected), StringView::npos) << expected;
	}
}
