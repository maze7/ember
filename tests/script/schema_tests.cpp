#include <ember/anim/components.h>
#include <ember/core/filesystem.h>
#include <ember/ecs/system.h>
#include <ember/net/replication.h>
#include <ember/net/serialize.h>
#include <ember/script/components.h>
#include <ember/script/host.h>
#include <ember/script/lua.h>
#include <ember/script/schema.h>

#include <gtest/gtest.h>

#include <glm/vec2.hpp>

#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

#if defined(EMBER_PLATFORM_WINDOWS)
	#include <process.h>
#else
	#include <unistd.h>
#endif

/**
 * The schema pass: components and prefabs declared in Luau, into the registry before the world, then
 * used by the scripts and the game like any C++ type; and what the running host says when a
 * declaration no longer matches what was registered.
 */
namespace schema_test
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

	inline constexpr auto DUMMY = ecs::prefab("dummy", Position{}, Health{}, Scratch{});

	void act_scripts(Host& host, ecs::Commands& commands) { host.simulate(static_cast<u8>(Stage::Act), commands); }
	void react_scripts(Host& host, ecs::Commands& commands) { host.simulate(static_cast<u8>(Stage::React), commands); }

	void bind(Binding& binding)
	{
		binding.expose<Position>();
		binding.expose<Health>();
		binding.expose<Scratch>({.derived = true});
		binding.stage("Act", static_cast<u8>(Stage::Act));
		binding.stage("React", static_cast<u8>(Stage::React));
		binding.constant("TILE", 16.0);
		binding.units(16.0, 60.0);
	}

	using File = std::pair<const char*, const char*>;

	class SchemaTest : public testing::Test
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

		/** The game's C++ content, the schema pass over these files, then a world whose host runs them. */
		void build(std::initializer_list<File> files, ecs::Role role = ecs::Role::Standalone)
		{
			m_registry.prefabs(DUMMY);
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

		[[nodiscard]] const Problem* schema_problem_with(StringView fragment) const
		{
			for (const Problem& problem : m_schema.problems)
				if (StringView(problem.message).find(fragment) != StringView::npos)
					return &problem;
			return nullptr;
		}

		[[nodiscard]] const ecs::ComponentInfo* type(StringView name) const { return m_registry.component_types().find(name); }

		[[nodiscard]] f64 field(const ecs::ComponentInfo& info, StringView name, const void* bytes) const
		{
			const ecs::FieldInfo* found = info.field(name);
			return found != nullptr && bytes != nullptr ? ecs::read_field(*found, bytes) : -12345.0;
		}

		[[nodiscard]] const void* bytes_of(const ecs::ComponentInfo& info, ecs::Entity entity) const
		{
			return info.find(info, m_world->registry, entity);
		}

		ecs::Registry m_registry;
		Vector<Source> m_sources{&memory::heap(MemoryTag::Scripting)};
		Schema m_schema;
		std::optional<ecs::World> m_world;
		Host* m_host = nullptr;
		Aliases m_aliases;
		u32 m_tick = 0;
	};

	TEST_F(SchemaTest, ComponentsDeclaredInLuauAreRegisteredWithTheirLayoutAndDefaults)
	{
		build({{"scripts/sim/components/hops.luau", R"(
			component "Hops" {
				kind = "Server",
				reach = tiles(2.5),
				hold = ticks(12),
				rest = seconds(0.5),
				legs = count(4),
				ready = true,
				way = vector.create(1, 0, 0),
				hopped = int(-3),
				weight = 1.5,
				last = tick(),
			}
			component "Glow" { kind = { "Interpolated", "Predicted" }, amount = 0.5 }
			component "Frenzied" { kind = "Server" }
		)"}});
		EXPECT_TRUE(m_schema.problems.empty()) << (m_schema.problems.empty() ? "" : m_schema.problems.front().message.c_str());
		ASSERT_EQ(m_schema.components.size(), 3u);
		EXPECT_EQ(m_schema.components[0].name, "Frenzied") << "by name";

		const ecs::ComponentInfo* hops = type("Hops");
		ASSERT_NE(hops, nullptr);
		EXPECT_TRUE(hops->dynamic);
		EXPECT_EQ(hops->kind, ecs::Kind::Server);
		ASSERT_EQ(hops->fields.size(), 9u);
		const char* order[] = {"hold", "hopped", "last", "legs", "reach", "ready", "rest", "way", "weight"};
		for (size_t i = 0; i < 9; ++i)
			EXPECT_EQ(hops->fields[i].name, order[i]) << i;

		const void* defaults = hops->defaults.data();
		EXPECT_EQ(hops->field("hold")->type, ecs::FieldType::U16);
		EXPECT_EQ(field(*hops, "hold", defaults), 12.0);
		EXPECT_EQ(hops->field("hopped")->type, ecs::FieldType::I32);
		EXPECT_EQ(field(*hops, "hopped", defaults), -3.0);
		EXPECT_EQ(hops->field("last")->type, ecs::FieldType::U32);
		EXPECT_EQ(field(*hops, "last", defaults), 0.0);
		EXPECT_EQ(hops->field("legs")->type, ecs::FieldType::U16);
		EXPECT_EQ(field(*hops, "legs", defaults), 4.0);
		EXPECT_EQ(hops->field("reach")->type, ecs::FieldType::F32);
		EXPECT_EQ(field(*hops, "reach", defaults), 40.0) << "2.5 tiles of 16 texels";
		EXPECT_EQ(hops->field("ready")->type, ecs::FieldType::Bool);
		EXPECT_EQ(field(*hops, "ready", defaults), 1.0);
		EXPECT_EQ(hops->field("rest")->type, ecs::FieldType::U16);
		EXPECT_EQ(field(*hops, "rest", defaults), 30.0) << "half a second of 60 ticks";
		EXPECT_EQ(hops->field("way")->type, ecs::FieldType::Vec2);
		EXPECT_EQ(field(*hops, "way", defaults), 1.0);
		EXPECT_EQ(hops->field("weight")->type, ecs::FieldType::F32);
		EXPECT_EQ(field(*hops, "weight", defaults), 1.5);

		const ecs::ComponentInfo* glow = type("Glow");
		ASSERT_NE(glow, nullptr);
		EXPECT_EQ(glow->kind, ecs::Kind::Interpolated | ecs::Kind::Predicted | ecs::Kind::Replicated | ecs::Kind::Sim);
		EXPECT_NE(glow->write, nullptr);
		EXPECT_NE(glow->interpolate, nullptr);

		const ecs::ComponentInfo* frenzied = type("Frenzied");
		ASSERT_NE(frenzied, nullptr);
		EXPECT_EQ(frenzied->size, 0u) << "a tag";

		// The running host knows them without being told: they are exposed by their fields.
		EXPECT_TRUE(m_host->problems().empty());
		EXPECT_NE(m_host->binding().exposed(hops->id), nullptr);
		EXPECT_EQ(m_host->binding().exposed(hops->id)->fields.size(), 9u);
	}

	TEST_F(SchemaTest, APrefabExtendsACppPrefabAndSpawnsByName)
	{
		build({{"scripts/sim/components/hops.luau", "component \"Hops\" { kind = \"Server\", reach = tiles(2.5), hold = ticks(12) }"},
			   {"scripts/sim/prefabs/hopper.luau", R"(
				prefab "hopper" {
					extends = "dummy",
					Hops = { reach = tiles(3) },
					Health = { current = 9 },
					Scratch = false,
				}
				prefab "big_hopper" { extends = "hopper", Hops = { hold = ticks(20) }, Scratch = {} }
			)"},
			   {"scripts/server/rules/spawn.luau", R"(
				system("Act", function()
					for e, health, hops in world:query(Health, Hops) do
						if health.current == 9 then
							health.current = hops.reach
							world:spawn("big_hopper", { Position = { value = vector.create(5, 6, 0) } })
						end
					end
				end)
			)"}});
		EXPECT_TRUE(m_schema.problems.empty()) << (m_schema.problems.empty() ? "" : m_schema.problems.front().message.c_str());
		ASSERT_EQ(m_schema.prefabs.size(), 2u);

		const ecs::Prefab* hopper = m_registry.prefab_types().find("hopper");
		ASSERT_NE(hopper, nullptr);
		const ecs::ComponentInfo* hops = type("Hops");
		ASSERT_NE(hops, nullptr);
		EXPECT_NE(hopper->find(m_registry.component_types().find<Position>()->id), nullptr) << "the base's";
		EXPECT_EQ(hopper->find(m_registry.component_types().find<Scratch>()->id), nullptr) << "taken away";
		ASSERT_NE(hopper->find(hops->id), nullptr);
		EXPECT_EQ(field(*hops, "reach", hopper->find(hops->id)->value.data()), 48.0);
		EXPECT_EQ(field(*hops, "hold", hopper->find(hops->id)->value.data()), 12.0) << "the type's default";
		Health health;
		std::memcpy(&health, hopper->find(m_registry.component_types().find<Health>()->id)->value.data(), sizeof(health));
		EXPECT_EQ(health.current, 9);
		EXPECT_EQ(health.most, 5);

		const ecs::Prefab* big = m_registry.prefab_types().find("big_hopper");
		ASSERT_NE(big, nullptr);
		EXPECT_EQ(field(*hops, "reach", big->find(hops->id)->value.data()), 48.0) << "hopper's";
		EXPECT_EQ(field(*hops, "hold", big->find(hops->id)->value.data()), 20.0) << "its own";
		EXPECT_NE(big->find(m_registry.component_types().find<Scratch>()->id), nullptr) << "put back";

		// Spawned from C++ by name, and from a server script.
		const ecs::Entity made = m_world->spawn(*hopper);
		ASSERT_NE(bytes_of(*hops, made), nullptr);
		EXPECT_FALSE(m_world->registry.all_of<Scratch>(made));
		tick();
		EXPECT_EQ(m_world->registry.get<Health>(made).current, 48) << "the script read the dynamic field";

		u32 bigs = 0;
		for (auto [entity, position] : m_world->registry.view<const Position>().each())
			if (position.value == glm::vec2(5.0f, 6.0f))
				++bigs;
		EXPECT_EQ(bigs, 1u);
		EXPECT_TRUE(m_host->problems().empty()) << (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(SchemaTest, GroupsStartAndEventsSwapComponents)
	{
		build({{"scripts/sim/components/boss.luau", R"(
				component "Hops" { kind = "Server", reach = tiles(2) }
				component "Frenzied" { kind = "Server" }
				component "Calm" { kind = "Server", patience = count(3) }
			)"},
			   {"scripts/sim/prefabs/boss.luau", R"(
				prefab "boss" {
					extends = "dummy",
					Hops = { reach = tiles(1) },
					groups = {
						calm = { Hops = { reach = tiles(2) }, Calm = {} },
						frenzied = { Hops = { reach = tiles(4) }, Frenzied = {} },
					},
					start = { "calm" },
					events = {
						frenzy = { add = { "frenzied" }, remove = { "calm" } },
						calm_down = { add = { "calm" }, remove = { "frenzied" } },
					},
				}
			)"},
			   {"scripts/server/rules/rage.luau", R"(
				system("Act", function()
					for e, s in world:query(Scratch) do
						if s.flag then
							s.flag = false
							e:event("frenzy")
						end
					end
				end)
			)"}});
		EXPECT_TRUE(m_schema.problems.empty()) << (m_schema.problems.empty() ? "" : m_schema.problems.front().message.c_str());

		const ecs::ComponentInfo* hops	   = type("Hops");
		const ecs::ComponentInfo* frenzied = type("Frenzied");
		const ecs::ComponentInfo* calm	   = type("Calm");
		ASSERT_NE(hops, nullptr);
		ASSERT_NE(frenzied, nullptr);
		ASSERT_NE(calm, nullptr);

		const ecs::Entity boss = m_world->spawn(*m_registry.prefab_types().find("boss"));
		EXPECT_EQ(field(*hops, "reach", bytes_of(*hops, boss)), 32.0) << "the start group's value over the prefab's";
		EXPECT_NE(bytes_of(*calm, boss), nullptr);
		EXPECT_EQ(bytes_of(*frenzied, boss), nullptr);

		// A script raises the event in Act, after the point's own work: the React point swaps the groups.
		m_world->registry.get<Scratch>(boss).flag = true;
		tick();
		EXPECT_EQ(field(*hops, "reach", bytes_of(*hops, boss)), 64.0);
		EXPECT_NE(bytes_of(*frenzied, boss), nullptr);
		EXPECT_EQ(bytes_of(*calm, boss), nullptr) << "calm's own component went with the group";

		// The game raises one from C++, between ticks.
		m_host->event(boss, "calm_down");
		tick();
		EXPECT_EQ(field(*hops, "reach", bytes_of(*hops, boss)), 32.0);
		EXPECT_EQ(bytes_of(*frenzied, boss), nullptr);
		EXPECT_NE(bytes_of(*calm, boss), nullptr);
		EXPECT_EQ(m_host->stats().events, 2u);
		EXPECT_TRUE(m_host->problems().empty()) << (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());
	}

	TEST_F(SchemaTest, ARunningDeclarationThatDiffersFromTheRegistryIsReportedOnce)
	{
		build({{"scripts/sim/components/hops.luau", "component \"Hops\" { kind = \"Server\", reach = tiles(2) }"},
			   {"scripts/sim/prefabs/hopper.luau", "prefab \"hopper\" { extends = \"dummy\", Hops = {} }"}});
		EXPECT_TRUE(m_host->problems().empty());

		// Saved while the game runs: a new type's value, a new type, a new prefab are for the next start; a
		// prefab's new value goes in live.
		Vector<Source> fresh(&memory::heap(MemoryTag::Scripting));
		fresh.push_back(make("scripts/sim/components/hops.luau", "component \"Hops\" { kind = \"Server\", reach = tiles(3) }"));
		fresh.push_back(make("scripts/sim/components/wings.luau", "component \"Wings\" { kind = \"Sim\", span = 2 }"));
		fresh.push_back(make("scripts/sim/prefabs/hopper.luau",
							 "prefab \"hopper\" { extends = \"dummy\", Hops = { reach = tiles(3) } }\nprefab \"flyer\" { extends = \"dummy\" }"));
		m_host->reload(fresh);

		ASSERT_NE(problem_with("component Hops changed (field 'reach' starts at another value): restart to apply it"), nullptr);
		EXPECT_EQ(problem_with("component Hops changed")->severity, Severity::Warning);
		EXPECT_EQ(problem_with("component Hops changed")->path, "scripts/sim/components/hops.luau");
		EXPECT_NE(problem_with("component Wings is new: restart to apply it"), nullptr);
		EXPECT_EQ(problem_with("prefab hopper changed"), nullptr) << "a value is tuned live, not reported";
		EXPECT_EQ(m_host->retuned().size(), 1u);
		EXPECT_NE(problem_with("prefab flyer is new: restart to apply it"), nullptr);
		EXPECT_EQ(m_host->stats().modules, 3u) << "warnings load";

		m_host->reload(fresh);
		EXPECT_EQ(problem_with("component Hops changed")->count, 1u) << "a reload clears the file's problems first";
		EXPECT_EQ(m_host->retuned().size(), 0u) << "the same values again retune nothing";
		EXPECT_EQ(type("Wings"), nullptr) << "nothing registers while the game runs";
	}

	TEST_F(SchemaTest, APrefabsNewValuesGoInLiveAndEntitiesStillAtTheOldOnesFollow)
	{
		build({{"scripts/sim/components/hops.luau",
				"component \"Hops\" { kind = \"Server\", reach = tiles(2), hopped = count(0) }"},
			   {"scripts/sim/prefabs/hopper.luau",
				"prefab \"hopper\" { extends = \"dummy\", Hops = { hopped = count(1) } }"}});
		const ecs::ComponentInfo& hops = *type("Hops");
		const ecs::Prefab& prefab	   = *m_registry.prefab_types().find("hopper");

		const ecs::Entity fresh = m_world->spawn(prefab);
		const ecs::Entity moved = m_world->spawn(prefab);
		ecs::write_field(*hops.field("hopped"), const_cast<void*>(bytes_of(hops, moved)),
						 7.0); // the game moved this one

		Vector<Source> saved(&memory::heap(MemoryTag::Scripting));
		saved.push_back(
			make("scripts/sim/prefabs/hopper.luau",
				 "prefab \"hopper\" { extends = \"dummy\", Hops = { reach = tiles(5), hopped = count(2) } }"));
		m_host->reload(saved);
		EXPECT_TRUE(m_host->problems().empty());
		ASSERT_EQ(m_host->retuned().size(), 1u);
		EXPECT_EQ(m_host->retuned()[0], prefab.id);

		// The world's prefab, and the entity still at the old values, carry the file's; a field the game moved keeps
		// its own, while the rest of its component follows.
		EXPECT_EQ(field(hops, "reach", m_world->prefab_of(prefab.id).find(hops.id)->value.data()), 80.0);
		EXPECT_EQ(field(hops, "reach", bytes_of(hops, fresh)), 80.0);
		EXPECT_EQ(field(hops, "hopped", bytes_of(hops, fresh)), 2.0);
		EXPECT_EQ(field(hops, "reach", bytes_of(hops, moved)), 80.0);
		EXPECT_EQ(field(hops, "hopped", bytes_of(hops, moved)), 7.0);
		EXPECT_EQ(field(hops, "reach", m_registry.prefab_types()[prefab.id].find(hops.id)->value.data()), 32.0)
			<< "the registry is as it was: every world retunes its own";

		// A new entity starts from the new values.
		const ecs::Entity later = m_world->spawn(prefab);
		EXPECT_EQ(field(hops, "reach", bytes_of(hops, later)), 80.0);
		EXPECT_EQ(field(hops, "hopped", bytes_of(hops, later)), 2.0);

		// Another component in the file is for the next start.
		saved.clear();
		saved.push_back(make("scripts/sim/prefabs/hopper.luau",
							 "prefab \"hopper\" { extends = \"dummy\", Hops = {}, Playing = {} }"));
		m_host->reload(saved);
		EXPECT_NE(problem_with("prefab hopper changed its components: restart to apply it"), nullptr);
		EXPECT_EQ(m_host->retuned().size(), 0u);
	}

	TEST_F(SchemaTest, EntityAndNameFieldsHoldAnotherEntityAndAName)
	{
		build({{"scripts/sim/components/target.luau",
				"component \"Target\" { kind = \"Server\", who = entity(), clip = name(\"idle\") }"},
			   {"scripts/sim/prefabs/seeker.luau",
				"prefab \"seeker\" { extends = \"dummy\", Target = {}, Playing = {} }"},
			   {"scripts/server/rules/seek.luau", R"(
				system("Act", function()
					local seekers = {}
					for e, t in world:query(Target) do
						table.insert(seekers, e)
					end
					table.sort(seekers, function(a, b) return a:id() < b:id() end)
					-- Each looks at the next; the last at the first. Read back at once, and through the other.
					for i, e in seekers do
						e.Target.who = seekers[i % #seekers + 1]
					end
					for i, e in seekers do
						local other = e.Target.who
						e.Scratch.flag = other ~= nil and other:id() == seekers[i % #seekers + 1]:id()
						e.Scratch.value = if tostring(e.Target.clip) == "idle" then 1 else 0
					end
					-- A name field takes a string, and hands its name to play().
					seekers[1].Target.clip = "squat"
					seekers[1]:play(seekers[1].Target.clip)
					seekers[1].Scratch.wide = if seekers[1].Target.clip == name("squat") then 1 else 0
				end)
			)"}});
		const ecs::ComponentInfo& target = *type("Target");
		ASSERT_EQ(target.field("who")->type, ecs::FieldType::Entity);
		ASSERT_EQ(target.field("clip")->type, ecs::FieldType::Name);
		EXPECT_EQ(ecs::read_field_bits(*target.field("clip"), target.defaults.data()), hash_text("idle"));

		const ecs::Prefab& prefab = *m_registry.prefab_types().find("seeker");
		const ecs::Entity a		  = m_world->spawn(prefab);
		const ecs::Entity b		  = m_world->spawn(prefab);
		const ecs::Entity c		  = m_world->spawn(prefab);
		m_world->registry.emplace<net::NetId>(a, net::NetId::make(5, 1));
		m_world->registry.emplace<net::NetId>(b, net::NetId::make(9, 1)); // c has no id yet
		tick();
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());

		const auto who = [&](ecs::Entity e) { return ecs::read_field_bits(*target.field("who"), bytes_of(target, e)); };
		for (const ecs::Entity e : {a, b, c})
		{
			EXPECT_TRUE(m_world->registry.get<Scratch>(e).flag) << "read back as the entity it was given";
			EXPECT_EQ(m_world->registry.get<Scratch>(e).value, 1u) << "a Name prints as its text";
		}
		EXPECT_EQ(who(a), net::NetId::make(9, 1).value);
		EXPECT_EQ(who(b), 0u) << "c has no network id yet: the ref waits";
		EXPECT_EQ(who(c), net::NetId::make(5, 1).value);
		EXPECT_EQ(m_world->registry.get<Scratch>(a).wide, 1.0);
		EXPECT_EQ(m_world->registry.get<anim::Playing>(a).clip, anim::name("squat"));
		EXPECT_EQ(ecs::read_field_bits(*target.field("clip"), bytes_of(target, a)), hash_text("squat"));

		// Once c has its id, the waiting ref is written at the next point.
		m_world->registry.emplace<net::NetId>(c, net::NetId::make(2, 1));
		tick();
		EXPECT_EQ(who(b), net::NetId::make(2, 1).value);
	}

	TEST_F(SchemaTest, MistakesInDeclarationsAreNamed)
	{
		build({{"scripts/sim/components/a.luau", "component \"Hops\" { kind = \"Server\", reach = tiles(2) }"},
			   {"scripts/sim/components/b.luau", "component \"Hops\" { kind = \"Server\", reach = tiles(3) }"},
			   {"scripts/sim/components/c.luau", "component \"Odd\" { kind = \"Sim\", reach = \"far\" }"},
			   {"scripts/sim/components/d.luau", "component \"Homeless\" { kind = { \"Server\", \"Client\" } }"},
			   {"scripts/sim/components/e.luau", "component \"Position\" { kind = \"Sim\" }"},
			   {"scripts/sim/components/f.luau", "component \"Whole\" { kind = \"Sim\", n = ticks(1.5) }"},
			   {"scripts/sim/prefabs/g.luau", "prefab \"orphan\" { extends = \"nothing\" }"},
			   {"scripts/sim/prefabs/h.luau", "prefab \"stranger\" { extends = \"dummy\", Nope = {} }"},
			   {"scripts/sim/prefabs/i.luau", "prefab \"dummy\" { }"},
			   {"scripts/sim/prefabs/j.luau", "prefab \"grouped\" { extends = \"dummy\", start = { \"nope\" } }"},
			   {"scripts/sim/prefabs/k.luau", "prefab \"loop\" { extends = \"loop\" }"},
			   {"scripts/sim/rules/l.luau", "system(\"Act\", function() component \"Late\" {} end)"}});

		EXPECT_NE(schema_problem_with("component Hops is declared twice"), nullptr);
		EXPECT_NE(schema_problem_with("field 'reach' takes a number, true or false, a vector, a unit value"), nullptr);
		EXPECT_NE(schema_problem_with("exactly one of Sim, Server and Client"), nullptr);
		EXPECT_NE(schema_problem_with("component Position: a C++ component has that name"), nullptr);
		EXPECT_NE(schema_problem_with("ticks() takes a whole number, not 1.5"), nullptr);
		EXPECT_NE(schema_problem_with("prefab orphan extends 'nothing', which is not a prefab"), nullptr);
		EXPECT_NE(schema_problem_with("prefab stranger: 'Nope' is not a component scripts know"), nullptr);
		EXPECT_NE(schema_problem_with("prefab dummy: a C++ prefab has that name"), nullptr);
		EXPECT_NE(schema_problem_with("start names group 'nope', which it has not"), nullptr);
		EXPECT_NE(schema_problem_with("prefab loop extends 'loop', which is not a prefab"), nullptr);
		ASSERT_NE(type("Hops"), nullptr) << "the first declaration stands";
		EXPECT_EQ(m_registry.prefab_types().find("orphan"), nullptr);

		tick();
		EXPECT_NE(problem_with("component is declared at a module's top level"), nullptr);
	}

	TEST_F(SchemaTest, UnitValuesDoArithmeticInTheSchemaPass)
	{
		build({{"scripts/sim/components/a.luau", R"(
				local REACH = tiles(2) * 2
				local HALF = tiles(2) / 2
				component "Measured" { kind = "Sim", reach = REACH, half = HALF, whole = ticks(4) + 1, far = tiles(1) < tiles(2) }
			)"}});
		EXPECT_TRUE(m_schema.problems.empty()) << (m_schema.problems.empty() ? "" : m_schema.problems.front().message.c_str());
		const ecs::ComponentInfo* measured = type("Measured");
		ASSERT_NE(measured, nullptr);
		EXPECT_EQ(measured->field("reach")->type, ecs::FieldType::F32) << "arithmetic leaves a plain number";
		EXPECT_EQ(field(*measured, "reach", measured->defaults.data()), 64.0);
		EXPECT_EQ(field(*measured, "half", measured->defaults.data()), 16.0);
		EXPECT_EQ(field(*measured, "whole", measured->defaults.data()), 5.0);
		EXPECT_EQ(measured->field("far")->type, ecs::FieldType::Bool);
		EXPECT_EQ(field(*measured, "far", measured->defaults.data()), 1.0);
	}

	TEST_F(SchemaTest, RightsFollowTheDeclaredKind)
	{
		build({{"scripts/sim/components/a.luau", R"(
				component "Tune" { kind = "Server", x = 1 }
				component "Pred" { kind = "Predicted", x = 1 }
				component "Rep" { kind = "Replicated", x = 1 }
			)"},
			   {"scripts/sim/prefabs/thing.luau", "prefab \"thing\" { extends = \"dummy\", Tune = {}, Pred = {}, Rep = {} }"},
			   {"scripts/sim/rules/a.luau", R"(
				system("Act", function()
					for e, p in world:query(Pred) do p.x = 2 end
					for e, t in world:query(Tune) do t.x = 3 end
				end)
			)"},
			   {"scripts/server/rules/b.luau", R"(
				system("React", function()
					for e, t, r in world:query(Tune, Rep) do t.x = 4; r.x = 5 end
				end)
			)"}});
		const ecs::Entity thing = m_world->spawn(*m_registry.prefab_types().find("thing"));
		tick();
		EXPECT_EQ(field(*type("Pred"), "x", bytes_of(*type("Pred"), thing)), 2.0) << "a sim script writes Predicted";
		EXPECT_EQ(field(*type("Tune"), "x", bytes_of(*type("Tune"), thing)), 4.0) << "the server wrote it; the sim was refused";
		EXPECT_EQ(field(*type("Rep"), "x", bytes_of(*type("Rep"), thing)), 5.0);
		ASSERT_NE(problem_with("Tune.x: a sim script writes Predicted components only"), nullptr);
		EXPECT_EQ(problem_with("Tune.x: a sim script writes")->path, "scripts/sim/rules/a.luau");
	}

	TEST_F(SchemaTest, DefinitionsDescribeWhatScriptsDeclared)
	{
		build({{"scripts/sim/components/hops.luau", "component \"Hops\" { kind = \"Server\", reach = tiles(2), ready = false, way = vector.create(1, 0, 0) }"}});

		String path(&memory::heap(MemoryTag::Engine));
		ASSERT_TRUE(fs::temporary_directory(path).has_value());
		ASSERT_TRUE(fs::join(path, path, "ember_schema_tests_" + std::to_string(getpid()) + ".d.luau").has_value());
		ASSERT_TRUE(m_host->write_definitions(path));
		const auto read = fs::read_file(path, memory::heap(MemoryTag::Engine));
		(void)fs::remove_file(path);
		ASSERT_TRUE(read.has_value());
		const StringView text(reinterpret_cast<const char*>(read->data()), read->size());

		for (const char* expected : {"declare extern type Hops with",
									 "\treach: number",
									 "\tready: boolean",
									 "\tway: vector",
									 "declare Hops: Component<Hops>",
									 "\tHops: Hops\n",
									 "declare extern type Stategraph with",
									 "\tfunction tick_of(self, mark: string, slot: number?): number",
									 "\tfunction go(self, state: string): ()",
									 "declare extern type Playing with",
									 "declare extern type Rng with",
									 "\trng: Rng",
									 "\tfunction play(self, clip: string | Name): ()",
									 "\tfunction event(self, name: string, args: (Entity | EventArgs)?): ()",
									 "declare extern type Shown with",
									 "\tfunction squash(self, x: number, y: number, seconds: number): ()",
									 "declare function show(prefab: string): (def: ShowDef) -> ()",
									 "declare extern type Name with",
									 "declare function name(text: string): Name",
									 "declare function entity(): Entity?",
									 "declare function component(name: string): (def: ComponentDef) -> ()",
									 "declare function stategraph(name: string): (def: GraphDef) -> Graph<any>",
									 "export type StateDef<S> = {",
									 "\tnext: (S | (e: Entity) -> ...S?)?,",
									 "export type Graph<S> = {",
									 "declare function to(state: string): (e: Entity, ...Event) -> ...any",
									 "\tfunction strike(self, def: StrikeDef): ()",
									 "\tstate: Stategraph",
									 "declare function tiles(x: number): number"})
			EXPECT_NE(text.find(expected), StringView::npos) << expected;
	}
}
