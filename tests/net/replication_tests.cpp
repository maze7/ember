#include <ember/ecs/system.h>
#include <ember/net/client.h>
#include <ember/net/loopback.h>
#include <ember/net/replica.h>
#include <ember/net/replicator.h>
#include <ember/net/server.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <random>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::net;
	using ecs::Entity;
	using ecs::NO_ENTITY;

	// ------------------------------------------------------------------ the game's components

	struct Position
	{
		f32 x = 0.0f;
		f32 y = 0.0f;

		bool operator==(const Position&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_float(stream, x);
			serialize_float(stream, y);
			return true;
		}
	};
	EMBER_COMPONENT(Position, Interpolated | Predicted);

	struct Health
	{
		u8 value = 100;

		bool operator==(const Health&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, value, 8);
			return true;
		}
	};
	EMBER_COMPONENT(Health, Replicated);

	struct Ammo
	{
		u8 rounds = 6; // a full magazine: a default that zeroed memory is not

		bool operator==(const Ammo&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, rounds, 8);
			return true;
		}
	};
	EMBER_COMPONENT(Ammo, OwnerOnly);

	/** A float the wire keeps to hundredths. */
	struct Drift
	{
		f32 value = 0.0f;

		bool operator==(const Drift&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_compressed_float(stream, value, -10.0f, 10.0f, 0.01f);
			return true;
		}
	};
	EMBER_COMPONENT(Drift, Replicated);

	/** A volley: everything a client needs to fire the same bullets. */
	struct Pattern
	{
		u8 shape   = 0;
		u32 seed   = 0;
		Tick start = NO_TICK;

		bool operator==(const Pattern&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, shape, 6);
			serialize_bits(stream, seed, 32);
			serialize_bits(stream, start, 32);
			return true;
		}
	};
	EMBER_COMPONENT(Pattern, Replicated);

	/** A status effect: no prefab has it, anything can catch it. */
	struct Burning
	{
		u8 damage	  = 6;
		u8 ticks_left = 120;

		bool operator==(const Burning&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, damage, 8);
			serialize_bits(stream, ticks_left, 8);
			return true;
		}
	};
	EMBER_COMPONENT(Burning, Replicated);

	/** A tag: having it is all there is to it, so it takes no bits and needs no serialize(). */
	struct Stunned
	{
		bool operator==(const Stunned&) const = default;
	};
	EMBER_COMPONENT(Stunned, Replicated);

	/** Private state the owner gains at run time. */
	struct Cooldown
	{
		u8 ticks = 0;

		bool operator==(const Cooldown&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, ticks, 8);
			return true;
		}
	};
	EMBER_COMPONENT(Cooldown, OwnerOnly);

	/** Where a turret points: drawn between updates like a position. */
	struct Aim
	{
		f32 degrees = 0.0f;

		bool operator==(const Aim&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_float(stream, degrees);
			return true;
		}
	};
	EMBER_COMPONENT(Aim, Interpolated);

	/** What never travels: the server's thinking, a client's looks. */
	struct Brain
	{
		f32 pace = 1.0f;
	};
	EMBER_COMPONENT(Brain, Server);

	struct Sprite
	{
		u16 frame = 0;
	};
	EMBER_COMPONENT(Sprite, Client);

	// Net ids: the Replicated components in the order the game registers them.
	constexpr ComponentId POSITION = 0;
	constexpr ComponentId HEALTH   = 1;
	constexpr ComponentId AMMO	   = 2;
	constexpr ComponentId DRIFT	   = 3;
	constexpr ComponentId PATTERN  = 4;
	constexpr ComponentId BURNING  = 5;
	constexpr ComponentId STUNNED  = 6;
	constexpr ComponentId COOLDOWN = 7;
	constexpr ComponentId AIM	   = 8;

	using Replicated = std::tuple<Position, Health, Ammo, Drift, Pattern, Burning, Stunned, Cooldown, Aim>;

	/** A crawler differs from an enemy's defaults. */
	constexpr Health CRAWLER_HEALTH = {.value = 30};
	constexpr Drift CRAWLER_DRIFT	= {.value = 1.234f};

	inline constexpr auto PLAYER =
		ecs::prefab("player", Position{}, Health{}, Ammo{}, Sprite{.frame = 9}, Priority{2.0f});
	inline constexpr auto ENEMY	  = ecs::prefab("enemy", Position{}, Health{}, Drift{}, Brain{}, Sprite{.frame = 2});
	inline constexpr auto VOLLEY  = ecs::prefab("volley", Pattern{}, Priority{4.0f});
	inline constexpr auto CRAWLER = ecs::variant(ENEMY, "crawler", CRAWLER_DRIFT, CRAWLER_HEALTH);
	inline constexpr auto SPAWNER = ecs::prefab("spawner", Brain{.pace = 60.0f}); // nothing Replicated

	constexpr PrefabId PLAYER_ID  = 0;
	constexpr PrefabId ENEMY_ID	  = 1;
	constexpr PrefabId VOLLEY_ID  = 2;
	constexpr PrefabId CRAWLER_ID = 3;

	[[nodiscard]] ComponentMask bits(std::initializer_list<ComponentId> ids)
	{
		ComponentMask mask = 0;
		for (const ComponentId id : ids)
			mask |= component_bit(id);
		return mask;
	}

	/** The registry of a game like a real one. Every machine builds its own, the same way. */
	struct Game
	{
		explicit Game(u32 entities = 1024) : max_entities(entities)
		{
			registry.components<Position, Health, Ammo, Drift, Pattern, Burning, Stunned, Cooldown, Aim>();
			registry.prefabs(PLAYER, ENEMY, VOLLEY, CRAWLER, SPAWNER);
		}

		ecs::Registry registry;
		u32 max_entities = 0;
	};

	/** The Replicated components an entity has, as a mask by net id. */
	[[nodiscard]] ComponentMask components(const entt::registry& registry, Entity entity)
	{
		if (!registry.valid(entity))
			return 0;

		ComponentMask mask = 0;
		[&]<size_t... Is>(std::index_sequence<Is...>)
		{
			((mask |= registry.all_of<std::tuple_element_t<Is, Replicated>>(entity) ? component_bit(Is) : 0), ...);
		}(std::make_index_sequence<std::tuple_size_v<Replicated>>{});
		return mask;
	}

	/** A component's value in a world; false when the entity lacks it. */
	template <class T> [[nodiscard]] bool value_in(const entt::registry& registry, Entity entity, T& out)
	{
		if (!registry.valid(entity) || !registry.all_of<T>(entity))
			return false;

		if constexpr (!std::is_empty_v<T>)
			out = registry.get<T>(entity);
		return true;
	}

	/** One client's end: its world, its replica, its packets on their way, and what it has seen. */
	struct Viewing
	{
		explicit Viewing(const Game& game)
			: world(game.registry, ecs::Role::Client), replica(world, {.max_entities = game.max_entities})
		{
			world.registry.on_construct<NetId>().connect<&Viewing::created>(*this);
			world.registry.on_destroy<NetId>().connect<&Viewing::removed>(*this);
		}

		ecs::World world;
		Replica replica;
		Sequence sequence = 0;
		std::deque<PacketNotice> notices; // owed to the replicator, oldest first
		std::vector<u32> bits;			  // each packet's section, in bits
		u32 creates = 0;
		u32 removes = 0;

		[[nodiscard]] Entity entity(NetId id) const { return replica.entity(id); }
		[[nodiscard]] bool alive(NetId id) const { return replica.entity(id) != NO_ENTITY; }
		[[nodiscard]] ComponentMask components(NetId id) const { return ::components(world.registry, entity(id)); }
		[[nodiscard]] Tick tick(NetId id) const { return replica.tick(entity(id)); }

		/** The server's word on a component: the samples' newest for those drawn or predicted, else the world's. */
		template <class T> [[nodiscard]] bool get(NetId id, T& out) const
		{
			const Entity found = entity(id);
			if constexpr (has_any(ecs::kind_of<T>, ecs::Kind::Interpolated | ecs::Kind::Predicted))
				return found != NO_ENTITY && world.registry.all_of<T>(found) && replica.server_value(found, out);
			else
				return value_in(world.registry, found, out);
		}

	private:
		void created(entt::registry&, Entity) { ++creates; }
		void removed(entt::registry&, Entity) { ++removes; }
	};

	/**
	 * A server world, its replicator and its viewers' worlds and replicas, joined by a scripted link.
	 * The test changes the server world as a game would; step() then updates the replicator for the
	 * tick, writes one packet per viewer, delivers or loses each, and tells the replicator what became
	 * of a packet `delay` packets after it: a round trip later.
	 */
	class Link
	{
	public:
		explicit Link(const Game& game, u32 viewers = 1, ReplicatorDef def = {}, u32 delay = 4)
			: m_game(game), m_world(game.registry, ecs::Role::Server), m_replicator(m_world, with(def, game)),
			  m_delay(delay)
		{
			for (u32 seat = 0; seat < viewers; ++seat)
				add_viewer();
		}

		[[nodiscard]] Replicator& server() { return m_replicator; }
		[[nodiscard]] ecs::World& world() { return m_world; }
		[[nodiscard]] Viewing& viewer(u32 seat = 0) { return *m_viewings[seat]; }
		[[nodiscard]] Tick tick() const { return m_tick; }

		/** The tick being simulated: what the next step() sends. */
		[[nodiscard]] Tick now() const { return m_tick + 1; }

		template <class Def, class... Ts> Entity spawn(const Def& def, const Ts&... overrides)
		{
			return m_world.spawn(def, overrides...);
		}

		template <class T> void set(Entity entity, const T& value) { m_world.set(entity, value); }
		template <class T> void remove(Entity entity) { m_world.registry.remove<T>(entity); }
		void destroy(Entity entity) { m_world.registry.destroy(entity); }

		[[nodiscard]] NetId id(Entity entity) const { return m_replicator.id(entity); }
		[[nodiscard]] ComponentMask components(Entity entity) const { return ::components(m_world.registry, entity); }

		/** Sends tick now()'s packets. lose: which viewers' packets the link drops, a bit each. */
		void step(u32 lose = 0)
		{
			++m_tick;
			m_replicator.update(m_tick);
			for (u32 seat = 0; seat < m_viewings.size(); ++seat)
				send(seat, (lose & (1u << seat)) != 0);
		}

		/** Steps until every notice has come back, with nothing lost: the world settles. */
		void settle(u32 steps = 0)
		{
			for (u32 i = 0; i < std::max(steps, m_delay + 2); ++i)
				step();
		}

		void add_viewer()
		{
			const u32 seat = static_cast<u32>(m_viewings.size());
			m_viewings.push_back(std::make_unique<Viewing>(m_game));
			m_replicator.add_viewer(static_cast<u8>(seat));
		}

	private:
		static ReplicatorDef with(ReplicatorDef def, const Game& game)
		{
			def.max_entities = game.max_entities;
			return def;
		}

		void send(u32 seat, bool lose)
		{
			Viewing& viewing = *m_viewings[seat];

			PacketBuffer buffer;
			serialize::WriteStream stream = packet_writer(buffer);
			m_replicator.write(static_cast<u8>(seat), stream, viewing.sequence, m_tick);
			viewing.bits.push_back(static_cast<u32>(stream.GetBitsProcessed()));
			stream.Flush();

			if (!lose)
			{
				PacketBuffer in;
				serialize::ReadStream reader;
				ASSERT_TRUE(packet_reader(reader, in, written(buffer, stream)));
				ASSERT_TRUE(viewing.replica.read(reader)) << "the replicator wrote a section that does not decode";
			}

			viewing.notices.push_back({.sequence = viewing.sequence, .delivered = !lose});
			++viewing.sequence;

			while (viewing.notices.size() > m_delay)
			{
				m_replicator.on_notice(static_cast<u8>(seat), viewing.notices.front());
				viewing.notices.pop_front();
			}
		}

		const Game& m_game;
		ecs::World m_world;
		Replicator m_replicator;
		std::vector<std::unique_ptr<Viewing>> m_viewings;
		u32 m_delay = 0;
		Tick m_tick = 0;
	};

	/** What a section of nothing costs: the tick and both counts. */
	constexpr u32 EMPTY_SECTION = 32 + 2 * SECTION_COUNT_BITS;

	/** Whether a viewer has a component exactly as the server has it, or lacks it as the server does. */
	template <class T> [[nodiscard]] bool same(Link& link, Entity entity, u32 seat = 0)
	{
		T server_value{};
		T client_value{};
		const bool server_has = value_in(link.world().registry, entity, server_value);
		const bool client_has = link.viewer(seat).get(link.id(entity), client_value);
		return server_has == client_has && (!server_has || server_value == client_value);
	}

	TEST(Replication, NetIdsPackAnIndexAndAGeneration)
	{
		const NetId id = NetId::make(5, 3);
		EXPECT_EQ(id.index(), 5u);
		EXPECT_EQ(id.generation(), 3u);
		EXPECT_TRUE(id);
		EXPECT_FALSE(NO_NET_ID);

		const NetId widest = NetId::make((1u << NetId::INDEX_BITS) - 1, NetId::MAX_GENERATION);
		EXPECT_EQ(widest.index(), (1u << NetId::INDEX_BITS) - 1);
		EXPECT_EQ(widest.generation(), NetId::MAX_GENERATION);
		EXPECT_NE(NetId::make(5, 3), NetId::make(5, 4));
	}

	TEST(Replication, VarintsTakeTheBitsTheyClaim)
	{
		for (const u32 value : {0u, 1u, 16u, 17u, 272u, 273u, 1000000u, 0xffffffffu})
		{
			PacketBuffer buffer;
			serialize::WriteStream writer = packet_writer(buffer);
			u32 written_value			  = value;
			ASSERT_TRUE(net::detail::serialize_varint(writer, written_value));
			EXPECT_EQ(writer.GetBitsProcessed(), net::detail::varint_bits(value)) << value;
			writer.Flush();

			PacketBuffer in;
			serialize::ReadStream reader;
			ASSERT_TRUE(packet_reader(reader, in, written(buffer, writer)));
			u32 read_value = 0;
			ASSERT_TRUE(net::detail::serialize_varint(reader, read_value));
			EXPECT_EQ(read_value, value);
		}

		EXPECT_EQ(net::detail::varint_bits(0), 1u);
		EXPECT_EQ(net::detail::varint_bits(1), 6u);
		EXPECT_EQ(net::detail::varint_bits(272), 11u);
		EXPECT_EQ(net::detail::varint_bits(273), 35u);
	}

	TEST(Replication, ASchemaReadsTheRegistry)
	{
		const Game game;
		const ecs::World world(game.registry, ecs::Role::Server);
		const Schema schema(world, game.max_entities);

		// The Replicated components alone, in registration order; the prefabs, all of them.
		EXPECT_EQ(schema.component_count(), 9u);
		EXPECT_EQ(schema.component(AIM).name, "Aim");
		EXPECT_EQ(schema.prefab_count(), 5u);
		EXPECT_TRUE(schema.replicated(CRAWLER_ID));
		EXPECT_FALSE(schema.replicated(4)) << "the spawner has nothing Replicated";

		EXPECT_EQ(schema.index_bits(), 10u) << "1024 entities";
		EXPECT_EQ(schema.prefab_bits(), 3u) << "5 prefabs";
		EXPECT_EQ(schema.component_bits(), 4u) << "9 component types";

		EXPECT_EQ(schema.owner_only(), bits({AMMO, COOLDOWN}));
		EXPECT_EQ(schema.interpolated(), bits({POSITION, AIM}));
		EXPECT_EQ(schema.predicted(), bits({POSITION}));
		EXPECT_EQ(schema.prefab(PLAYER_ID).components, bits({POSITION, HEALTH, AMMO}));
		EXPECT_EQ(schema.prefab(PLAYER_ID).shared, bits({POSITION, HEALTH}));

		// Each prefab value as the bits it writes, whatever order the prefab listed it in.
		EXPECT_EQ(schema.prefab_wire(ENEMY_ID, POSITION).bits, 64u);
		EXPECT_EQ(schema.prefab_wire(VOLLEY_ID, PATTERN).bits, 6u + 32u + 32u);
		EXPECT_EQ(schema.prefab_wire(CRAWLER_ID, HEALTH).bytes[0], 30);
	}

	TEST(Replication, AnEntityIsCreatedChangedAndRemovedOnTheClient)
	{
		const Game game;
		Link link(game);

		const Entity enemy = link.spawn(ENEMY, Position{.x = 10, .y = 20}, Health{.value = 80});
		EXPECT_FALSE(link.id(enemy)) << "it replicates from the next update";
		link.step();

		const NetId id = link.id(enemy);
		ASSERT_TRUE(id);
		EXPECT_EQ(link.world().registry.get<NetId>(enemy), id);
		EXPECT_EQ(link.server().entity(id), enemy);

		// The client's half of its prefab, with the server's values over it.
		Viewing& viewer		= link.viewer();
		const Entity mirror = viewer.entity(id);
		ASSERT_NE(mirror, NO_ENTITY);
		const entt::registry& client = viewer.world.registry;
		EXPECT_EQ(client.get<NetId>(mirror), id);
		EXPECT_EQ(client.get<ecs::PrefabRef>(mirror).id, ENEMY_ID);
		EXPECT_EQ(client.get<Sprite>(mirror).frame, 2) << "its Client components, from the prefab";
		EXPECT_FALSE(client.all_of<Brain>(mirror)) << "and none of the server's";
		EXPECT_FALSE(client.all_of<ecs::Simulated>(mirror)) << "this client does not own it";
		EXPECT_FALSE(client.all_of<Owned>(mirror));
		EXPECT_EQ(client.get<Position>(mirror), (Position{.x = 10, .y = 20}));
		EXPECT_EQ(client.get<Health>(mirror).value, 80);
		EXPECT_EQ(viewer.tick(id), 1u);
		EXPECT_EQ(viewer.components(id), bits({POSITION, HEALTH, DRIFT}));
		EXPECT_EQ(viewer.creates, 1u);

		link.set(enemy, Position{.x = 11, .y = 20});
		link.set(enemy, Health{.value = 79});
		link.step();
		Position position;
		ASSERT_TRUE(viewer.get(id, position));
		EXPECT_EQ(position.x, 11.0f);
		EXPECT_EQ(client.get<Health>(mirror).value, 79);

		link.destroy(enemy);
		link.step();
		EXPECT_EQ(link.server().entity(id), NO_ENTITY);
		EXPECT_FALSE(viewer.alive(id));
		EXPECT_FALSE(client.valid(mirror));
		EXPECT_EQ(viewer.replica.entity_count(), 0u);
		EXPECT_EQ(viewer.removes, 1u);
	}

	TEST(Replication, OnlyEntitiesWithSomethingReplicatedTravel)
	{
		const Game game;
		Link link(game);
		const Entity spawner = link.spawn(SPAWNER);
		const Entity loose	 = link.world().registry.create(); // made by hand, from no prefab
		link.set(loose, Health{});
		link.step();

		EXPECT_FALSE(link.id(spawner));
		EXPECT_FALSE(link.id(loose));
		EXPECT_EQ(link.server().entity_count(), 0u);
		EXPECT_EQ(link.viewer().bits.back(), EMPTY_SECTION);
	}

	TEST(Replication, PrefabValuesNeverTravel)
	{
		const Game game;
		Link link(game);
		const Entity crawler = link.spawn(CRAWLER);
		link.step();

		// The create names the prefab and nothing follows it: index, create, prefab, generation, owned
		// and key, age, previous, set, and a mask of three empty bits.
		EXPECT_EQ(link.viewer().bits.back(), EMPTY_SECTION + 10 + 1 + 3 + 12 + 2 + 1 + 1 + 1 + 3);

		Health health;
		ASSERT_TRUE(link.viewer().get(link.id(crawler), health));
		EXPECT_EQ(health.value, 30);

		// The client has what the server has, to the bit: both made it from the same prefab.
		EXPECT_TRUE(same<Drift>(link, crawler));
		EXPECT_TRUE(same<Position>(link, crawler));
	}

	TEST(Replication, AStatusEffectComesAndGoes)
	{
		const Game game;
		Link link(game);
		const Entity enemy = link.spawn(ENEMY);
		link.settle();
		const NetId id = link.id(enemy);

		link.set(enemy, Burning{.damage = 9, .ticks_left = 60});
		link.step();

		Burning burning;
		ASSERT_TRUE(link.viewer().get(id, burning));
		EXPECT_EQ(burning, (Burning{.damage = 9, .ticks_left = 60}));
		EXPECT_EQ(link.viewer().components(id), link.components(enemy));

		// It ticks down like any component.
		link.world().registry.get<Burning>(enemy).ticks_left = 59;
		link.step();
		ASSERT_TRUE(link.viewer().get(id, burning));
		EXPECT_EQ(burning.ticks_left, 59);

		link.remove<Burning>(enemy);
		link.step();
		EXPECT_FALSE(link.viewer().get(id, burning));
		EXPECT_EQ(link.viewer().components(id), bits({POSITION, HEALTH, DRIFT}));

		// Back to its prefab's set: the record says so in two bits, and once it arrives nothing is owed.
		link.settle();
		EXPECT_EQ(link.viewer().bits.back(), EMPTY_SECTION);
	}

	TEST(Replication, ASetIsToldWholeSoNoLossCanSplitIt)
	{
		const Game game;
		Link link(game, 1, {}, 4);
		const Entity crawler = link.spawn(CRAWLER);
		link.settle();

		// Three changes to what it has, each in a packet that is lost.
		link.set(crawler, Burning{.damage = 3});
		link.step(1);
		link.remove<Health>(crawler);
		link.step(1);
		link.set(crawler, Stunned{});
		link.step(1);

		link.settle();
		EXPECT_EQ(link.viewer().components(link.id(crawler)), bits({POSITION, DRIFT, BURNING, STUNNED}));
		EXPECT_TRUE(same<Burning>(link, crawler));
		EXPECT_TRUE(same<Health>(link, crawler)) << "gone on both";
	}

	TEST(Replication, AComponentLostAndRegainedComesBackWithItsValue)
	{
		const Game game;
		Link link(game);
		const Entity crawler = link.spawn(CRAWLER);
		link.settle();
		const NetId id = link.id(crawler);

		link.remove<Health>(crawler);
		link.step();
		Health health;
		EXPECT_FALSE(link.viewer().get(id, health));

		// Regained at the prefab's own value: it still travels, since the client no longer has it.
		link.set(crawler, CRAWLER_HEALTH);
		link.step();
		ASSERT_TRUE(link.viewer().get(id, health));
		EXPECT_EQ(health.value, 30);

		// Lost and regained at another value inside one round trip of lost packets.
		link.remove<Health>(crawler);
		link.step(1);
		link.set(crawler, Health{.value = 7});
		link.step(1);
		link.settle();
		ASSERT_TRUE(link.viewer().get(id, health));
		EXPECT_EQ(health.value, 7);
	}

	TEST(Replication, PrivateComponentsGainedAtRunTimeStayPrivate)
	{
		const Game game;
		Link link(game, 2);
		const Entity player = link.spawn(PLAYER, Owner{.seat = 0});
		link.settle();
		const NetId id = link.id(player);

		link.set(player, Cooldown{.ticks = 30});
		link.step();

		Cooldown cooldown;
		ASSERT_TRUE(link.viewer(0).get(id, cooldown));
		EXPECT_EQ(cooldown.ticks, 30);
		EXPECT_FALSE(link.viewer(1).get(id, cooldown));
		EXPECT_EQ(link.viewer(1).bits.back(), EMPTY_SECTION) << "nothing the other viewer gets changed";
		EXPECT_EQ(link.viewer(1).components(id), bits({POSITION, HEALTH}));

		link.remove<Cooldown>(player);
		link.step();
		EXPECT_FALSE(link.viewer(0).get(id, cooldown));
		EXPECT_EQ(link.viewer(1).bits.back(), EMPTY_SECTION);
	}

	TEST(Replication, ATagCostsOnlyItsPlaceInTheSet)
	{
		const Game game;
		Link link(game);
		const Entity enemy = link.spawn(ENEMY);
		link.settle();

		link.set(enemy, Stunned{});
		link.step();

		// Index, update, age, a previous change of 0, the set's one id, a mask of four: no value bits.
		const u32 gap = link.tick() - 1 - 1;
		EXPECT_EQ(link.viewer().bits.back(), EMPTY_SECTION + 10 + 1 + 1 + 1 + net::detail::varint_bits(gap) + 1 +
												 net::detail::varint_bits(1) + 4 + 4);

		Stunned stunned;
		EXPECT_TRUE(link.viewer().get(link.id(enemy), stunned));
	}

	TEST(Replication, InterpolationDrawsWhatTheClientDoesNotOwn)
	{
		const Game game;
		Link link(game);
		const Entity enemy = link.spawn(ENEMY); // tick 1
		for (i32 tick = 1; tick <= 5; ++tick)
		{
			link.set(enemy, Position{.x = static_cast<f32>(tick * 10)});
			link.step();
		}

		Viewing& viewer		= link.viewer();
		const Entity mirror = viewer.entity(link.id(enemy));

		// Values that are drawn wait for interpolate(); whatever it was given last stays until then.
		viewer.replica.interpolate(2.5);
		EXPECT_FLOAT_EQ(viewer.world.registry.get<Position>(mirror).x, 25.0f);

		link.set(enemy, Position{.x = 999});
		link.step();
		EXPECT_FLOAT_EQ(viewer.world.registry.get<Position>(mirror).x, 25.0f) << "a packet does not move it";

		viewer.replica.interpolate(4.25);
		EXPECT_FLOAT_EQ(viewer.world.registry.get<Position>(mirror).x, 42.5f);

		viewer.replica.interpolate(100.0);
		EXPECT_FLOAT_EQ(viewer.world.registry.get<Position>(mirror).x, 999.0f) << "nothing made up past the newest";
	}

	TEST(Replication, TheOwnerPredictsAndHearsTheServersWord)
	{
		const Game game;
		Link link(game, 2);
		const Entity player = link.spawn(PLAYER, Owner{.seat = 0, .key = 77}, Position{.x = 5});
		link.settle();
		const NetId id = link.id(player);

		// The owner's copy is Simulated and carries its key; the other client's is neither.
		const entt::registry& mine	 = link.viewer(0).world.registry;
		const entt::registry& theirs = link.viewer(1).world.registry;
		const Entity own			 = link.viewer(0).entity(id);
		const Entity other			 = link.viewer(1).entity(id);
		EXPECT_TRUE(mine.all_of<ecs::Simulated>(own));
		EXPECT_EQ(mine.get<Owned>(own).key, 77u);
		EXPECT_FALSE(theirs.all_of<ecs::Simulated>(other));
		EXPECT_FALSE(theirs.all_of<Owned>(other));
		EXPECT_EQ(mine.get<Position>(own).x, 5.0f) << "a new entity starts at the server's value";

		// The owner moves ahead of the server; the server's word comes back beside it, not over it.
		link.viewer(0).world.registry.get<Position>(own).x = 7.0f;
		link.set(player, Position{.x = 6});
		link.step();
		link.viewer(0).replica.interpolate(static_cast<f64>(link.tick()));

		EXPECT_EQ(mine.get<Position>(own).x, 7.0f) << "prediction owns it";
		Position server;
		ASSERT_TRUE(link.viewer(0).replica.server_value(own, server));
		EXPECT_EQ(server.x, 6.0f);
		EXPECT_EQ(link.viewer(0).replica.tick(own), link.tick());

		// Everything else of its own lands as it comes: it predicts only what is Predicted.
		link.set(player, Health{.value = 50});
		link.step();
		EXPECT_EQ(mine.get<Health>(own).value, 50);
	}

	TEST(Replication, AnInterpolatedComponentGainedLaterIsNotSlidInFromItsDefault)
	{
		const Game game;
		Link link(game);
		const Entity enemy = link.spawn(ENEMY); // tick 1

		// Moving from tick 1 to 5; a turret's aim appears at 6 and turns at 7.
		for (i32 tick = 1; tick <= 5; ++tick)
		{
			link.set(enemy, Position{.x = static_cast<f32>(tick * 10)});
			link.step();
		}

		link.set(enemy, Aim{.degrees = 90});
		link.step();
		link.set(enemy, Aim{.degrees = 100});
		link.step();

		Viewing& viewer		= link.viewer();
		const Entity mirror = viewer.entity(link.id(enemy));

		viewer.replica.interpolate(3.5);
		EXPECT_EQ(viewer.world.registry.get<Aim>(mirror).degrees, 90.0f)
			<< "at its first value as far back as samples go";
		EXPECT_EQ(viewer.world.registry.get<Position>(mirror).x, 35.0f) << "the position's samples untouched by it";

		viewer.replica.interpolate(6.5);
		EXPECT_EQ(viewer.world.registry.get<Aim>(mirror).degrees, 95.0f);
	}

	TEST(Replication, AnotherEntityLeavingKeepsYourSamples)
	{
		// Samples are packed per component type: when one goes, another moves into its place, samples and all.
		const Game game;
		Link link(game);
		const Entity first	= link.spawn(ENEMY);
		const Entity second = link.spawn(ENEMY);
		for (i32 tick = 1; tick <= 4; ++tick)
		{
			link.set(first, Position{.x = static_cast<f32>(-tick)});
			link.set(second, Position{.x = static_cast<f32>(tick * 10)});
			link.step();
		}

		const NetId gone = link.id(first);
		link.destroy(first);
		link.step();
		ASSERT_FALSE(link.viewer().alive(gone));

		link.viewer().replica.interpolate(2.5);
		EXPECT_EQ(link.viewer().world.registry.get<Position>(link.viewer().entity(link.id(second))).x, 25.0f);
	}

	TEST(Replication, EverythingBeforeTheFirstPacketArrivesAsOne)
	{
		const Game game;
		Link link(game);
		const Entity enemy = link.spawn(ENEMY);
		link.remove<Drift>(enemy);
		link.set(enemy, Burning{.damage = 4});
		link.set(enemy, Health{.value = 50});
		link.step();

		EXPECT_EQ(link.viewer().components(link.id(enemy)), bits({POSITION, HEALTH, BURNING}));
		EXPECT_TRUE(same<Health>(link, enemy));
		EXPECT_TRUE(same<Burning>(link, enemy));
		EXPECT_TRUE(same<Drift>(link, enemy)) << "gone on both";
	}

	TEST(Replication, EveryRecordLeavesTheClientExactlyAsTheServerWas)
	{
		// A busy world on a bad link: a third of the packets lost, the news of each a round trip late,
		// and status effects coming and going all the while.
		const Game game;
		Link link(game, 1, {}, 6);
		std::mt19937 random(7);
		std::vector<Entity> entities;
		u32 spawned = 0;

		const auto check = [&](Entity entity, u32 step)
		{
			const NetId id = link.id(entity);
			ASSERT_EQ(link.viewer().components(id), link.components(entity) & ~bits({AMMO, COOLDOWN}))
				<< "step " << step;
			ASSERT_TRUE(same<Position>(link, entity)) << "step " << step;
			ASSERT_TRUE(same<Health>(link, entity)) << "step " << step;
			ASSERT_TRUE(same<Drift>(link, entity)) << "step " << step;
			ASSERT_TRUE(same<Burning>(link, entity)) << "step " << step;
			ASSERT_TRUE(same<Stunned>(link, entity)) << "step " << step;
		};

		for (u32 step = 0; step < 600; ++step)
		{
			if (entities.size() < 40 || random() % 8 == 0)
			{
				entities.push_back(random() % 2 == 0 ? link.spawn(ENEMY) : link.spawn(CRAWLER));
				++spawned;
			}

			if (entities.size() > 10 && random() % 10 == 0)
			{
				const u32 victim = random() % entities.size();
				link.destroy(entities[victim]);
				entities.erase(entities.begin() + victim);
			}

			for (const Entity entity : entities)
			{
				const ComponentMask has = link.components(entity);
				if (random() % 3 == 0)
					link.set(entity, Position{.x = static_cast<f32>(random() % 1000), .y = 0});
				if ((has & component_bit(HEALTH)) != 0 && random() % 7 == 0)
					link.set(entity, Health{.value = static_cast<u8>(random() % 200)});

				switch (random() % 40)
				{
					case 0:
						link.set(entity, Burning{.damage = static_cast<u8>(random() % 9)});
						break;
					case 1:
						link.remove<Burning>(entity);
						break;
					case 2:
						link.set(entity, Stunned{});
						break;
					case 3:
						link.remove<Stunned>(entity);
						break;
					case 4:
						link.remove<Health>(entity);
						break;
					case 5:
						link.set(entity, Health{.value = static_cast<u8>(random() % 200)});
						break;
					default:
						break;
				}
			}

			link.step(random() % 3 == 0 ? 1 : 0);

			// Whatever this packet brought is the server's entity as it stands right now.
			for (const Entity entity : entities)
			{
				const NetId id = link.id(entity);
				if (link.viewer().alive(id) && link.viewer().tick(id) == link.tick())
					check(entity, step);
			}
		}

		// Once the link heals, the client's world is the server's.
		link.settle(20);
		EXPECT_EQ(link.viewer().replica.entity_count(), entities.size());
		for (const Entity entity : entities)
			check(entity, 600);

		EXPECT_EQ(link.viewer().creates - link.viewer().removes, entities.size());
		EXPECT_LE(link.viewer().creates, spawned);
	}

	TEST(Replication, AnUnchangedWorldCostsOnlyTheSectionHeader)
	{
		const Game game;
		Link link(game);
		for (u32 i = 0; i < 100; ++i)
			(void)link.spawn(ENEMY);

		link.settle(40);
		EXPECT_EQ(link.viewer().replica.entity_count(), 100u);

		// Every packet of a round trip and more: not one record.
		for (u32 i = 0; i < 10; ++i)
		{
			link.step();
			EXPECT_EQ(link.viewer().bits.back(), EMPTY_SECTION);
		}
	}

	TEST(Replication, AChangeTooSmallForTheWireIsNoChange)
	{
		const Game game;
		Link link(game);
		const Entity enemy = link.spawn(ENEMY, Drift{.value = 1.0f});
		link.settle();

		link.set(enemy, Drift{.value = 1.001f});
		link.step();
		EXPECT_EQ(link.viewer().bits.back(), EMPTY_SECTION) << "a thousandth is below the wire's hundredths";

		link.set(enemy, Drift{.value = 1.02f});
		link.step();
		EXPECT_GT(link.viewer().bits.back(), EMPTY_SECTION);

		Drift drift;
		ASSERT_TRUE(link.viewer().get(link.id(enemy), drift));
		EXPECT_NEAR(drift.value, 1.02f, 0.006f);
	}

	TEST(Replication, ARecordIsSentAgainOnlyWhenItsPacketIsLost)
	{
		const Game game;
		Link link(game, 1, {}, 4);
		const Entity enemy = link.spawn(ENEMY);
		link.settle();

		link.set(enemy, Health{.value = 50});
		link.step(1); // lost: nobody knows yet
		const u32 record = link.viewer().bits.back();
		EXPECT_GT(record, EMPTY_SECTION);

		// While its fate is unknown, the record is not repeated. The notice comes back as the fourth
		// packet after it leaves.
		for (u32 i = 0; i < 4; ++i)
		{
			link.step();
			EXPECT_EQ(link.viewer().bits.back(), EMPTY_SECTION);
		}

		// Lost: the very next packet carries it again, five ticks older, so its age takes 6 bits, not 1.
		link.step();
		EXPECT_EQ(link.viewer().bits.back(), record + 5);

		Health health;
		ASSERT_TRUE(link.viewer().get(link.id(enemy), health));
		EXPECT_EQ(health.value, 50);

		link.settle();
		EXPECT_EQ(link.viewer().bits.back(), EMPTY_SECTION);
	}

	TEST(Replication, CreatesAndRemovalsRepeatUntilTheyArrive)
	{
		const Game game;
		Link link(game, 1, {}, 2);
		const Entity volley = link.spawn(VOLLEY, Pattern{.shape = 3, .seed = 99, .start = 10});

		for (u32 i = 0; i < 6; ++i)
			link.step(1);
		const NetId id = link.id(volley);
		EXPECT_FALSE(link.viewer().alive(id));

		link.step();
		ASSERT_TRUE(link.viewer().alive(id));
		Pattern pattern;
		ASSERT_TRUE(link.viewer().get(id, pattern));
		EXPECT_EQ(pattern, (Pattern{.shape = 3, .seed = 99, .start = 10}));

		link.settle();
		link.destroy(volley);
		for (u32 i = 0; i < 6; ++i)
			link.step(1);
		EXPECT_TRUE(link.viewer().alive(id));

		link.step();
		EXPECT_FALSE(link.viewer().alive(id));
		EXPECT_EQ(link.viewer().creates, 1u);
		EXPECT_EQ(link.viewer().removes, 1u);
	}

	TEST(Replication, RecordsFillTheBudgetByPriorityAndNothingStarves)
	{
		const Game game;
		Link link(game, 1, {.max_bits = 600});

		std::vector<Entity> enemies;
		for (u32 i = 0; i < 40; ++i)
			enemies.push_back(link.spawn(ENEMY));
		const Entity boss = link.spawn(PLAYER); // its prefab's Priority is 2

		std::vector<Tick> last_seen(41, NO_TICK);
		u32 longest_wait  = 0;
		u32 boss_records  = 0;
		u32 enemy_records = 0;

		for (u32 step = 0; step < 300; ++step)
		{
			for (u32 i = 0; i < enemies.size(); ++i)
				link.set(enemies[i], Position{.x = static_cast<f32>(step), .y = static_cast<f32>(i)});
			link.set(boss, Position{.x = static_cast<f32>(step), .y = -1});

			link.step();
			ASSERT_LE(link.viewer().bits.back(), 600u) << "the budget caps the section";

			for (u32 i = 0; i <= enemies.size(); ++i)
			{
				const NetId id = link.id(i < enemies.size() ? enemies[i] : boss);
				if (link.viewer().alive(id) && link.viewer().tick(id) == link.tick())
				{
					if (step >= 50)
					{
						longest_wait = std::max(longest_wait, link.tick() - last_seen[i]);
						(i < enemies.size() ? enemy_records : boss_records) += 1;
					}
					last_seen[i] = link.tick();
				}
			}
		}

		EXPECT_EQ(link.viewer().replica.entity_count(), 41u);
		EXPECT_LT(longest_wait, 20u) << "everyone's turn comes";

		// The boss counts twice an enemy: it goes about twice as often as any one enemy.
		const f32 per_enemy = static_cast<f32>(enemy_records) / 40.0f;
		EXPECT_NEAR(static_cast<f32>(boss_records) / per_enemy, 2.0f, 0.5f);

		// Priority is the entity's own: an enraged enemy outranks the boss.
		link.set(enemies[0], Priority{8.0f});
		u32 enraged	 = 0;
		boss_records = 0;
		for (u32 step = 0; step < 200; ++step)
		{
			for (u32 i = 0; i < enemies.size(); ++i)
				link.set(enemies[i], Position{.x = static_cast<f32>(step), .y = static_cast<f32>(i + 1000)});
			link.set(boss, Position{.x = static_cast<f32>(step), .y = -2});
			link.step();

			enraged += link.viewer().tick(link.id(enemies[0])) == link.tick() ? 1 : 0;
			boss_records += link.viewer().tick(link.id(boss)) == link.tick() ? 1 : 0;
		}
		EXPECT_GT(enraged, boss_records * 3);
	}

	TEST(Replication, RelevanceTakesAnEntityOutOfOneViewersWorldAndBack)
	{
		const Game game;
		Link link(game, 2);
		const Entity enemy = link.spawn(ENEMY, Health{.value = 7});
		link.settle();
		const NetId id = link.id(enemy);
		ASSERT_TRUE(link.viewer(0).alive(id) && link.viewer(1).alive(id));

		link.server().set_relevance(0, id, 0.0f);
		link.set(enemy, Health{.value = 8});
		link.settle();
		EXPECT_FALSE(link.viewer(0).alive(id));
		EXPECT_TRUE(link.viewer(1).alive(id)) << "viewer 1 still cares";

		link.set(enemy, Health{.value = 9});
		link.set(enemy, Burning{});
		link.server().set_relevance(0, id, 1.0f);
		link.settle();
		ASSERT_TRUE(link.viewer(0).alive(id));

		Health health;
		ASSERT_TRUE(link.viewer(0).get(id, health));
		EXPECT_EQ(health.value, 9) << "back with its whole current state";
		EXPECT_EQ(link.viewer(0).components(id), link.components(enemy)) << "and what it gained meanwhile";
		EXPECT_EQ(link.viewer(0).creates, 2u);
		EXPECT_EQ(link.viewer(0).removes, 1u);
	}

	TEST(Replication, OnlyTheOwnerGetsItsPrivateComponentsItsKeyAndEveryPacket)
	{
		const Game game;
		Link link(game, 2);
		const Entity player = link.spawn(PLAYER, Owner{.seat = 0, .key = 77}, Ammo{.rounds = 30});
		link.settle();
		const NetId id = link.id(player);

		Ammo ammo;
		ASSERT_TRUE(link.viewer(0).get(id, ammo));
		EXPECT_EQ(ammo.rounds, 30);
		EXPECT_FALSE(link.viewer(1).get(id, ammo)) << "not even the prefab's: it has none to anyone else";
		EXPECT_EQ(link.viewer(1).components(id), bits({POSITION, HEALTH}));
		EXPECT_FALSE(link.viewer(1).world.registry.all_of<Owned>(link.viewer(1).entity(id)))
			<< "the key is the owner's business";

		// A private change concerns the owner alone.
		link.set(player, Ammo{.rounds = 29});
		link.step();
		EXPECT_EQ(link.viewer(1).bits.back(), EMPTY_SECTION);
		ASSERT_TRUE(link.viewer(0).get(id, ammo));
		EXPECT_EQ(ammo.rounds, 29);

		// The owner hears of its entity in every packet, changed or not.
		for (u32 i = 0; i < 5; ++i)
		{
			link.step();
			EXPECT_EQ(link.viewer(0).tick(id), link.tick());
		}
		EXPECT_LT(link.viewer(1).tick(id), link.tick());
	}

	TEST(Replication, AnIndexReturnsOnlyOnceEveryViewerHasLetGo)
	{
		const Game game(4);
		Link link(game, 2, {}, 2);

		std::vector<Entity> entities;
		for (u32 i = 0; i < 4; ++i)
			entities.push_back(link.spawn(ENEMY));
		const Entity extra = link.spawn(ENEMY);
		link.settle();
		EXPECT_FALSE(link.id(extra)) << "every index is taken";
		link.destroy(extra);

		const NetId old_id = link.id(entities[1]);
		link.destroy(entities[1]);
		for (u32 i = 0; i < 6; ++i)
			link.step(0b10); // viewer 1 hears nothing
		EXPECT_FALSE(link.viewer(0).alive(old_id));
		EXPECT_TRUE(link.viewer(1).alive(old_id));
		const Entity early = link.spawn(ENEMY);
		link.step(0b10);
		EXPECT_FALSE(link.id(early)) << "viewer 1 may still have it";
		link.destroy(early);

		link.settle(10);
		const Entity late = link.spawn(ENEMY);
		link.step();
		const NetId reused = link.id(late);
		ASSERT_TRUE(reused);
		EXPECT_EQ(reused.index(), old_id.index());
		EXPECT_EQ(reused.generation(), old_id.generation() + 1);
		EXPECT_EQ(link.server().entity(old_id), NO_ENTITY) << "the old id stays dead";
	}

	TEST(Replication, TheOldestFreeIndexIsReusedFirst)
	{
		const Game game(4);
		Link link(game, 0);

		const Entity a = link.spawn(ENEMY);
		const Entity b = link.spawn(ENEMY);
		(void)link.spawn(ENEMY);
		link.step();
		const NetId a_id = link.id(a);
		const NetId b_id = link.id(b);

		link.destroy(b);
		link.step();
		link.destroy(a);
		link.step();

		const Entity fresh = link.spawn(ENEMY);
		link.step();
		const Entity second = link.spawn(ENEMY);
		link.step();
		const Entity third = link.spawn(ENEMY);
		link.step();
		const Entity fourth = link.spawn(ENEMY);
		link.step();

		EXPECT_EQ(link.id(fresh).index(), 3u) << "never used yet";
		EXPECT_EQ(link.id(second), NetId::make(b_id.index(), 2)) << "freed first";
		EXPECT_EQ(link.id(third), NetId::make(a_id.index(), 2));
		EXPECT_FALSE(link.id(fourth));
		EXPECT_EQ(link.server().entity_count(), 4u);
	}

	TEST(Replication, AViewerWhoJoinsLateGetsTheWholeWorld)
	{
		const Game game;
		Link link(game, 1);
		std::vector<Entity> entities;
		for (u32 i = 0; i < 20; ++i)
		{
			entities.push_back(link.spawn(ENEMY, Position{.x = static_cast<f32>(i), .y = 5}));
			if (i % 4 == 0)
				link.set(entities.back(), Stunned{});
			link.step();
		}

		link.add_viewer();
		link.settle();
		EXPECT_EQ(link.viewer(1).replica.entity_count(), 20u);
		EXPECT_EQ(link.viewer(1).creates, 20u);

		Position position;
		ASSERT_TRUE(link.viewer(1).get(link.id(entities[13]), position));
		EXPECT_EQ(position, (Position{.x = 13, .y = 5}));
		EXPECT_EQ(link.viewer(1).components(link.id(entities[12])), link.components(entities[12]));
		EXPECT_EQ(link.viewer(1).components(link.id(entities[13])), link.components(entities[13]));
	}

	TEST(Replication, AViewerThatLeavesLetsGoOfEverything)
	{
		const Game game(2);
		Link link(game, 1, {}, 2);
		const Entity a = link.spawn(ENEMY);
		(void)link.spawn(ENEMY);
		link.settle();

		// The removal never arrives: the seat empties instead.
		link.destroy(a);
		link.step(1);
		const Entity waiting = link.spawn(ENEMY);
		link.step(1);
		EXPECT_FALSE(link.id(waiting));
		link.destroy(waiting);

		link.server().remove_viewer(0);
		const Entity next = link.spawn(ENEMY);
		link.server().update(link.now());
		EXPECT_TRUE(link.id(next)) << "no viewer holds the index";
	}

	TEST(Replication, WhoeverTakesASeatNextOwnsNothingOfTheLast)
	{
		const Game game;
		Link link(game, 1);
		const Entity player = link.spawn(PLAYER, Owner{.seat = 0, .key = 5}, Ammo{.rounds = 12});
		link.settle();
		ASSERT_TRUE(link.viewer().world.registry.all_of<Owned>(link.viewer().entity(link.id(player))));

		// The seat's client leaves and another sits down before the game has cleared the old player away.
		link.server().remove_viewer(0);
		link.server().add_viewer(0);
		Viewing newcomer(game);

		PacketBuffer buffer;
		serialize::WriteStream stream = packet_writer(buffer);
		link.server().write(0, stream, 0, link.now());
		stream.Flush();

		PacketBuffer in;
		serialize::ReadStream reader;
		ASSERT_TRUE(packet_reader(reader, in, written(buffer, stream)));
		ASSERT_TRUE(newcomer.replica.read(reader));

		const Entity mirror = newcomer.entity(link.id(player));
		ASSERT_NE(mirror, NO_ENTITY);
		EXPECT_FALSE(newcomer.world.registry.all_of<Owned>(mirror));
		EXPECT_FALSE(newcomer.world.registry.all_of<ecs::Simulated>(mirror));
		EXPECT_FALSE(newcomer.world.registry.all_of<Ammo>(mirror)) << "the last client's private state stays private";
	}

	TEST(Replication, ASectionThatDoesNotDecodeChangesNothing)
	{
		const Game game;
		Link link(game);
		const Entity enemy = link.spawn(ENEMY, Health{.value = 1});
		link.settle();
		const NetId id = link.id(enemy);

		// The next packet, cut short.
		link.set(enemy, Health{.value = 2});
		link.set(enemy, Burning{});
		const Entity newcomer = link.spawn(ENEMY);
		link.server().update(link.now());

		PacketBuffer buffer;
		serialize::WriteStream stream = packet_writer(buffer);
		link.server().write(0, stream, 1000, link.now());
		stream.Flush();
		const Span<const u8> whole = written(buffer, stream);

		PacketBuffer in;
		serialize::ReadStream reader;
		ASSERT_TRUE(packet_reader(reader, in, whole.first(whole.size() - 2)));
		EXPECT_FALSE(link.viewer().replica.read(reader));

		Health health;
		ASSERT_TRUE(link.viewer().get(id, health));
		EXPECT_EQ(health.value, 1) << "not half applied";
		EXPECT_EQ(link.viewer().components(id), bits({POSITION, HEALTH, DRIFT}));
		EXPECT_FALSE(link.viewer().alive(link.id(newcomer)));
		EXPECT_EQ(link.viewer().replica.latest_tick(), link.tick());

		// Whole, it applies.
		ASSERT_TRUE(packet_reader(reader, in, whole));
		EXPECT_TRUE(link.viewer().replica.read(reader));
		EXPECT_TRUE(link.viewer().alive(link.id(newcomer)));
		Burning burning;
		EXPECT_TRUE(link.viewer().get(id, burning));
	}

	/**
	 * A section with one create record of an enemy, written by hand: what a broken or hostile server
	 * might send. differ is the set as the record tells it; components is what that comes to, and
	 * carried the components whose values follow, each at its default.
	 */
	[[nodiscard]] bool read_create(Viewing& viewing, bool owned, std::initializer_list<u32> differ,
								   ComponentMask components, ComponentMask carried)
	{
		const Schema schema(viewing.world, 1024);

		PacketBuffer buffer;
		serialize::WriteStream stream = packet_writer(buffer);
		stream.SerializeBits(10u, 32); // tick
		stream.SerializeBits(0u, static_cast<int>(SECTION_COUNT_BITS));
		stream.SerializeBits(1u, static_cast<int>(SECTION_COUNT_BITS));

		stream.SerializeBits(3u, static_cast<int>(schema.index_bits()));
		stream.SerializeBits(1u, 1); // create
		stream.SerializeBits(ENEMY_ID, static_cast<int>(schema.prefab_bits()));
		stream.SerializeBits(1u, static_cast<int>(NetId::GENERATION_BITS));
		stream.SerializeBits(owned ? 1u : 0u, 1);
		stream.SerializeBits(0u, 1); // no key
		stream.SerializeBits(0u, 1); // age 0
		stream.SerializeBits(0u, 1); // no previous change

		stream.SerializeBits(1u, 1); // the set follows
		u32 count = static_cast<u32>(differ.size());
		(void)net::detail::serialize_varint(stream, count);
		for (const u32 id : differ)
			stream.SerializeBits(id, static_cast<int>(schema.component_bits()));

		ComponentMask mask = 0;
		u32 place		   = 0;
		for (ComponentMask left = components; left != 0; ++place)
		{
			if ((carried & component_bit(net::detail::take_lowest(left))) != 0)
				mask |= ComponentMask{1} << place;
		}
		(void)net::detail::serialize_mask(stream, mask, net::detail::component_count(components));

		for (ComponentMask left = carried; left != 0;)
		{
			const ecs::ComponentInfo& info = schema.component(net::detail::take_lowest(left));
			(void)info.write(stream, info.defaults.data());
		}
		stream.Flush();

		PacketBuffer in;
		serialize::ReadStream reader;
		return packet_reader(reader, in, written(buffer, stream)) && viewing.replica.read(reader);
	}

	TEST(Replication, AMalformedSetIsRefused)
	{
		const Game game;
		const ComponentMask enemy = bits({POSITION, HEALTH, DRIFT});

		const ComponentMask burning		  = bits({BURNING});
		const ComponentMask private_state = bits({COOLDOWN});

		// Well formed: a burning enemy, and one that lost its drift.
		{
			Viewing viewing(game);
			EXPECT_TRUE(read_create(viewing, false, {BURNING}, enemy | burning, burning));
			EXPECT_EQ(viewing.components(NetId::make(3, 1)), enemy | burning);

			Viewing other(game);
			EXPECT_TRUE(read_create(other, false, {DRIFT}, enemy & ~bits({DRIFT}), 0));
			EXPECT_EQ(other.components(NetId::make(3, 1)), enemy & ~bits({DRIFT}));
		}

		// A component gained without its value.
		{
			Viewing viewing(game);
			EXPECT_FALSE(read_create(viewing, false, {BURNING}, enemy | burning, 0));
			EXPECT_EQ(viewing.replica.entity_count(), 0u);
			EXPECT_TRUE(viewing.world.registry.view<NetId>().empty()) << "nothing made in the world";
		}

		// Out of order, or one component twice.
		{
			Viewing viewing(game);
			const ComponentMask both = burning | bits({STUNNED});
			EXPECT_FALSE(read_create(viewing, false, {STUNNED, BURNING}, enemy | both, both));
			EXPECT_FALSE(read_create(viewing, false, {BURNING, BURNING}, enemy | burning, burning));
			EXPECT_EQ(viewing.replica.entity_count(), 0u);
		}

		// A component type the schema does not have.
		{
			Viewing viewing(game);
			EXPECT_FALSE(read_create(viewing, false, {9}, enemy, 0));
		}

		// Another client's private component, on an entity this client does not own.
		{
			Viewing viewing(game);
			EXPECT_FALSE(read_create(viewing, false, {COOLDOWN}, enemy | private_state, private_state));
			EXPECT_TRUE(read_create(viewing, true, {COOLDOWN}, enemy | private_state, private_state))
				<< "its own, it may have";
		}
	}

	template <u32 N> struct Numbered
	{
		u8 value = N;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, value, 8);
			return true;
		}
	};

	template <u32 N> consteval auto ember_component(const Numbered<N>*) noexcept
	{
		return ecs::ComponentDescription<Numbered<N>>{"Numbered", ecs::Kind::Sim | ecs::Kind::Replicated};
	}

	template <u32... Ns> constexpr auto wide_prefab(std::integer_sequence<u32, Ns...>)
	{
		return ecs::prefab("wide", Numbered<Ns>{}...);
	}

	inline constexpr auto WIDE = wide_prefab(std::make_integer_sequence<u32, 36>{});

	template <u32... Ns> void register_numbered(ecs::Registry& registry, std::integer_sequence<u32, Ns...>)
	{
		registry.components<Numbered<Ns>...>();
	}

	TEST(Replication, AnEntityWithMoreThanThirtyTwoComponentsCrossesTheWire)
	{
		// Forty component types, the masks wider than one 32 bit write.
		ecs::Registry registry;
		register_numbered(registry, std::make_integer_sequence<u32, 40>{});
		registry.prefabs(WIDE);

		ecs::World server(registry, ecs::Role::Server);
		ecs::World client(registry, ecs::Role::Client);
		Replicator replicator(server, {.max_viewers = 1, .max_entities = 64});
		replicator.add_viewer(0);
		Replica replica(client, {.max_entities = 64});

		const Entity entity = server.spawn(WIDE, Numbered<34>{.value = 200}, Numbered<39>{.value = 7});
		server.registry.remove<Numbered<2>>(entity);
		replicator.update(1);

		PacketBuffer buffer;
		serialize::WriteStream stream = packet_writer(buffer);
		replicator.write(0, stream, 0, 1);
		stream.Flush();

		PacketBuffer in;
		serialize::ReadStream reader;
		ASSERT_TRUE(packet_reader(reader, in, written(buffer, stream)));
		ASSERT_TRUE(replica.read(reader));

		const Entity mirror = replica.entity(replicator.id(entity));
		ASSERT_NE(mirror, NO_ENTITY);
		EXPECT_EQ(client.registry.get<Numbered<34>>(mirror).value, 200);
		EXPECT_EQ(client.registry.get<Numbered<39>>(mirror).value, 7);
		EXPECT_EQ(client.registry.get<Numbered<35>>(mirror).value, 35) << "the prefab's, untouched";
		EXPECT_FALSE(client.registry.all_of<Numbered<2>>(mirror));
		EXPECT_FALSE(client.registry.all_of<Numbered<38>>(mirror));
	}

	TEST(Replication, SamplesMarkWhenEachStateBegan)
	{
		const Game game;
		Link link(game);
		const Entity enemy = link.spawn(ENEMY); // tick 1

		// Moving every tick to tick 5, then still until tick 15, then off again at 16.
		for (i32 x = 1; x <= 15; ++x)
		{
			if (x <= 5)
				link.set(enemy, Position{.x = static_cast<f32>(x * 10), .y = 0});
			link.step();
		}
		link.set(enemy, Position{.x = 200, .y = 0}); // tick 16
		link.step();

		Viewing& viewer		= link.viewer();
		const Entity mirror = viewer.entity(link.id(enemy));
		const auto drawn	= [&](f64 tick)
		{
			viewer.replica.interpolate(tick);
			return viewer.world.registry.get<Position>(mirror).x;
		};

		EXPECT_FLOAT_EQ(drawn(3.5), 35.0f);

		// Still from 5 until 15: the start at 16 is drawn at 16, not smeared from 5.
		EXPECT_FLOAT_EQ(drawn(10.0), 50.0f);
		EXPECT_FLOAT_EQ(drawn(15.25), 87.5f);

		// Nothing is made up past the newest or before the oldest.
		EXPECT_FLOAT_EQ(drawn(30.0), 200.0f);
		EXPECT_FLOAT_EQ(drawn(0.5), 10.0f);
	}

	TEST(Replication, AStateTheClientMissedIsDrawnAcrossTheGap)
	{
		const Game game;
		Link link(game);
		const Entity enemy = link.spawn(ENEMY); // tick 1
		link.step();

		// Changes at 2 and 3 are lost; only 4's arrives. What came between is unknown: blend across it.
		link.set(enemy, Position{.x = 10, .y = 0});
		link.step(1);
		link.set(enemy, Position{.x = 20, .y = 0});
		link.step(1);
		link.set(enemy, Position{.x = 30, .y = 0});
		link.step();

		link.viewer().replica.interpolate(2.5);
		EXPECT_FLOAT_EQ(link.viewer().world.registry.get<Position>(link.viewer().entity(link.id(enemy))).x, 15.0f);
	}

	TEST(Replication, ADeadIdIsRefusedEverywhere)
	{
		const Game game(2);
		Link link(game);
		const Entity first = link.spawn(ENEMY);
		link.settle();
		const NetId old_id = link.id(first);
		link.destroy(first);
		link.settle();

		const Entity second = link.spawn(ENEMY);
		link.settle();
		ASSERT_EQ(link.id(second).index(), old_id.index() == 0 ? 1u : 0u);

		const Entity third = link.spawn(ENEMY);
		link.settle();
		ASSERT_EQ(link.id(third).index(), old_id.index());

		// The first entity's id names nothing, though its index holds the third.
		EXPECT_EQ(link.server().entity(old_id), NO_ENTITY);
		EXPECT_FALSE(link.viewer().alive(old_id));
		link.server().set_relevance(0, old_id, 0.0f);
		link.settle();
		EXPECT_TRUE(link.viewer().alive(link.id(third))) << "the dead id's relevance went nowhere";

		Health health;
		EXPECT_FALSE(link.viewer().get(old_id, health));
		ASSERT_TRUE(link.viewer().get(link.id(third), health));
		EXPECT_EQ(health.value, 100);
	}

	TEST(Replication, ANewEntityStartsRelevantToEveryone)
	{
		// The index's last entity was out of viewer 0's world; the next one to take it is not.
		const Game game(2);
		Link link(game);
		(void)link.spawn(ENEMY);
		const Entity hidden = link.spawn(ENEMY);
		link.step();
		const NetId hidden_id = link.id(hidden);
		link.server().set_relevance(0, hidden_id, 0.0f);
		link.settle();
		EXPECT_FALSE(link.viewer().alive(hidden_id));

		link.destroy(hidden);
		link.step();
		const Entity next = link.spawn(ENEMY);
		link.settle();
		ASSERT_EQ(link.id(next).index(), hidden_id.index());
		EXPECT_TRUE(link.viewer().alive(link.id(next)));
	}

	TEST(Replication, ASectionOlderThanTheLastIsRefused)
	{
		const Game game;
		Link link(game);
		(void)link.spawn(ENEMY);
		link.settle();

		PacketBuffer buffer;
		serialize::WriteStream stream = packet_writer(buffer);
		link.server().write(0, stream, 500, link.tick() - 1);
		stream.Flush();

		PacketBuffer in;
		serialize::ReadStream reader;
		ASSERT_TRUE(packet_reader(reader, in, written(buffer, stream)));
		EXPECT_FALSE(link.viewer().replica.read(reader)) << "a server never goes back in time";
		EXPECT_EQ(link.viewer().replica.latest_tick(), link.tick());
	}

	TEST(Replication, AChangeIsSampledWhenItHappenedNotWhenItArrived)
	{
		const Game game;
		Link link(game, 1, {}, 4);
		const Entity enemy = link.spawn(ENEMY); // at 0 from tick 1
		link.step();

		link.set(enemy, Position{.x = 10}); // tick 2, lost
		link.step(1);
		for (u32 i = 0; i < 5; ++i)
			link.step();
		const NetId id = link.id(enemy);
		ASSERT_EQ(link.viewer().tick(id), 7u) << "heard of again at 7, once the loss came back";

		link.viewer().replica.interpolate(1.5);
		EXPECT_FLOAT_EQ(link.viewer().world.registry.get<Position>(link.viewer().entity(id)).x, 5.0f)
			<< "it moved at 2";
	}

	enum class Stage : u8
	{
		Move,
	};

	void walk_system(ecs::Sim<Position, const Brain> walkers)
	{
		for (auto [entity, position, brain] : walkers.each())
			position.x += brain.pace;
	}

	TEST(Replication, ValuesASystemWritesTravel)
	{
		// The game's rules, run by the server world: the replicator needs no word from them.
		ecs::Registry registry;
		registry.components<Position, Health, Ammo, Drift, Pattern, Burning, Stunned, Cooldown, Aim>();
		registry.prefabs(PLAYER, ENEMY, VOLLEY, CRAWLER, SPAWNER);
		registry.simulate<walk_system>(Stage::Move);

		ecs::World server(registry, ecs::Role::Server);
		ecs::World client(registry, ecs::Role::Client);
		Replicator replicator(server, {.max_viewers = 1});
		replicator.add_viewer(0);
		Replica replica(client);

		const Entity enemy = server.spawn(ENEMY);
		for (Tick tick = 1; tick <= 3; ++tick)
		{
			server.run(ecs::Phase::Simulate);
			replicator.update(tick);

			PacketBuffer buffer;
			serialize::WriteStream stream = packet_writer(buffer);
			replicator.write(0, stream, static_cast<Sequence>(tick), tick);
			stream.Flush();

			PacketBuffer in;
			serialize::ReadStream reader;
			ASSERT_TRUE(packet_reader(reader, in, written(buffer, stream)));
			ASSERT_TRUE(replica.read(reader));
		}

		Position position;
		ASSERT_TRUE(replica.server_value(replica.entity(replicator.id(enemy)), position));
		EXPECT_EQ(position.x, 3.0f);
	}

	TEST(Replication, AReplicatorMadeLateStillFindsWhatTheWorldSpawned)
	{
		const Game game;
		ecs::World server(game.registry, ecs::Role::Server);
		const Entity early = server.spawn(ENEMY);

		Replicator replicator(server, {.max_viewers = 1});
		replicator.update(1);
		EXPECT_TRUE(replicator.id(early));
		EXPECT_EQ(replicator.entity_count(), 1u);
	}

	TEST(Replication, AResetClearsTheClientsWorldOfEverythingReplicated)
	{
		const Game game;
		Link link(game);
		for (u32 i = 0; i < 3; ++i)
			(void)link.spawn(ENEMY);
		link.settle();

		Viewing& viewer	   = link.viewer();
		const Entity local = viewer.world.registry.create(); // the client's own: an effect, a menu
		ASSERT_EQ(viewer.replica.entity_count(), 3u);

		viewer.replica.reset();
		EXPECT_EQ(viewer.replica.entity_count(), 0u);
		EXPECT_TRUE(viewer.world.registry.view<NetId>().empty());
		EXPECT_TRUE(viewer.world.registry.valid(local));
		EXPECT_EQ(viewer.replica.latest_tick(), NO_TICK);
	}

	// ------------------------------------------------------------------ through the endpoints

	constexpr f64 DT = 1.0 / 60.0;

	struct Input
	{
		u8 buttons = 0;

		bool operator==(const Input&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, buttons, 2);
			return true;
		}
	};

	/** One client machine: its transport, its world and replica, and the client between them. */
	struct Machine
	{
		Machine(LoopbackNetwork& network, const Game& game)
			: transport(network), viewing(std::make_unique<Viewing>(game))
		{
			client.emplace(transport, ClientDef{.commands = command_codec<Input>(), .replica = &viewing->replica});
		}

		LoopbackTransport transport;
		std::unique_ptr<Viewing> viewing;
		std::optional<Client> client;
		Tick tick = NO_TICK;
	};

	/** A server world with a replicator and some clients, ticking at 60 Hz with the server on each tick. */
	class Session
	{
	public:
		Session(const Game& game, u32 clients, const LinkConditions& link = {})
			: m_world(game.registry, ecs::Role::Server), m_server_end(m_network),
			  m_replicator(m_world, {.max_viewers = 4, .max_entities = game.max_entities})
		{
			m_server_end.set_conditions(link);
			m_server.emplace(m_server_end, ServerDef{.commands	  = command_codec<Input>(),
													 .max_clients = 4,
													 .queue		  = {.tick_seconds = DT},
													 .replicator  = &m_replicator});
			EXPECT_TRUE(m_server->listen("server"));

			for (u32 i = 0; i < clients; ++i)
			{
				m_machines.push_back(std::make_unique<Machine>(m_network, game));
				m_machines.back()->transport.set_conditions(link);
				EXPECT_TRUE(m_machines.back()->client->connect("server", m_now));
			}
		}

		[[nodiscard]] ecs::World& world() { return m_world; }
		[[nodiscard]] Replicator& replicator() { return m_replicator; }
		[[nodiscard]] Machine& machine(u32 i) { return *m_machines[i]; }
		[[nodiscard]] f64 time() const { return m_now; }
		[[nodiscard]] Tick tick() const { return m_tick; }

		/** Runs ticks until `seconds` more have passed; each tick, `each` plays the server world. */
		template <class Each> void run(f64 seconds, Each&& each)
		{
			for (const f64 end = m_now + seconds; m_now < end;)
			{
				m_now += DT;
				const Tick tick = m_tick + 1;

				ServerEvent event;
				while (m_server->poll(tick, m_now, event))
				{
				}

				each(tick);
				m_tick = tick;
				m_server->send_packets(m_now);

				for (const std::unique_ptr<Machine>& machine : m_machines)
					frame(*machine);
			}
		}

		void run(f64 seconds)
		{
			run(seconds, [](Tick) {});
		}

	private:
		void frame(Machine& machine)
		{
			if (!machine.client)
				return;

			ClientEvent event;
			while (machine.client->poll(m_now, event))
			{
				if (event.kind == ClientEventKind::Joined)
					machine.tick = event.tick;
			}

			if (machine.client->state() == ClientState::Playing)
				machine.client->send_command(++machine.tick, Input{}, 0.0f, m_now);
		}

		ecs::World m_world;
		LoopbackNetwork m_network;
		LoopbackTransport m_server_end;
		Replicator m_replicator;
		std::optional<Server> m_server;
		std::vector<std::unique_ptr<Machine>> m_machines;
		f64 m_now	= 1.0;
		Tick m_tick = 100;
	};

	TEST(Replication, EntitiesReachEveryClientThroughTheServer)
	{
		const Game game;
		Session session(game, 2, {.latency = 0.03, .jitter = 0.01, .loss = 0.1f});

		std::vector<Entity> entities;
		session.run(0.1,
					[&](Tick)
					{
						if (entities.empty())
							for (u32 i = 0; i < 50; ++i)
								entities.push_back(session.world().spawn(ENEMY));
					});

		session.run(3.0,
					[&](Tick tick)
					{
						for (u32 i = 0; i < entities.size(); ++i)
						{
							session.world().set(entities[i],
												Position{.x = static_cast<f32>(tick), .y = static_cast<f32>(i)});
							if ((tick + i) % 50 == 0)
								session.world().set(entities[i], Burning{});
							if ((tick + i) % 50 == 25)
								session.world().registry.remove<Burning>(entities[i]);
						}
					});

		// The world stops; a moment later every client has all of it, exactly.
		session.run(1.0);
		for (u32 c = 0; c < 2; ++c)
		{
			const Viewing& viewing = *session.machine(c).viewing;
			ASSERT_EQ(viewing.replica.entity_count(), 50u) << "client " << c;
			for (const Entity entity : entities)
			{
				const NetId id = session.replicator().id(entity);
				Position client_position;
				ASSERT_TRUE(viewing.get(id, client_position));
				EXPECT_EQ(client_position, session.world().registry.get<Position>(entity));
				EXPECT_EQ(viewing.components(id), components(session.world().registry, entity));
			}

			EXPECT_GT(viewing.replica.latest_tick(), 100u);
		}
	}

	TEST(Replication, ARejoiningClientStartsFromAnEmptyWorld)
	{
		const Game game;
		Session session(game, 1, {.latency = 0.02});

		Entity kept	   = NO_ENTITY;
		Entity dropped = NO_ENTITY;
		session.run(0.05,
					[&](Tick)
					{
						if (kept == NO_ENTITY)
						{
							kept	= session.world().spawn(ENEMY);
							dropped = session.world().spawn(ENEMY);
						}
					});
		session.run(1.0);
		Machine& machine = session.machine(0);
		ASSERT_EQ(machine.viewing->replica.entity_count(), 2u);
		const NetId kept_id	   = session.replicator().id(kept);
		const NetId dropped_id = session.replicator().id(dropped);

		// It leaves; while it is away one entity goes, and it comes back to the world as it is now.
		machine.client->disconnect(DisconnectReason::Requested, session.time());
		session.world().registry.destroy(dropped);
		session.run(0.5);
		ASSERT_TRUE(machine.client->connect("server", session.time()));
		session.run(1.0);

		EXPECT_EQ(machine.viewing->replica.entity_count(), 1u);
		EXPECT_TRUE(machine.viewing->alive(kept_id));
		EXPECT_FALSE(machine.viewing->alive(dropped_id));
		EXPECT_EQ(machine.viewing->world.registry.view<NetId>().size(), 1u) << "the old session's entities are gone";
	}

	TEST(Replication, AThousandMovingEntitiesStillFitOnePacketAtATime)
	{
		const Game game(2048);
		Session session(game, 1, {.latency = 0.02});

		std::vector<Entity> entities;
		session.run(4.0,
					[&](Tick tick)
					{
						if (entities.empty())
							for (u32 i = 0; i < 1000; ++i)
								entities.push_back(session.world().spawn(ENEMY));

						for (u32 i = 0; i < entities.size(); ++i)
							session.world().set(entities[i],
												Position{.x = static_cast<f32>(tick % 1000), .y = static_cast<f32>(i)});
					});

		// Too much to send every tick, so the entities take turns; every one keeps arriving.
		const Viewing& viewing = *session.machine(0).viewing;
		EXPECT_EQ(viewing.replica.entity_count(), 1000u);
		for (const Entity entity : entities)
			EXPECT_GE(viewing.tick(session.replicator().id(entity)) + 60, viewing.replica.latest_tick())
				<< "updated within the last second";
	}

	TEST(Replication, AClientThatLeavesLetsGoOfItsEntities)
	{
		// Both indices in use. When the client goes, the one it saw destroyed is free at once, though
		// its removal never reached anyone.
		const Game game(2);
		Session session(game, 1, {.latency = 0.02});

		Entity first = NO_ENTITY;
		session.run(0.05,
					[&](Tick)
					{
						if (first == NO_ENTITY)
						{
							first = session.world().spawn(ENEMY);
							(void)session.world().spawn(ENEMY);
						}
					});
		session.run(1.0);
		ASSERT_EQ(session.machine(0).viewing->replica.entity_count(), 2u);

		session.machine(0).client.reset();
		session.run(0.5);

		session.world().registry.destroy(first);
		const Entity next = session.world().spawn(ENEMY);
		session.replicator().update(session.tick() + 1);
		EXPECT_TRUE(session.replicator().id(next));
	}

	TEST(Replication, AClientNeverAcknowledgesAPacketWhoseEntitiesDoNotDecode)
	{
		const Game game;
		LoopbackNetwork network;
		LoopbackTransport server_end(network);
		LoopbackTransport client_end(network);
		ASSERT_TRUE(server_end.listen("raw"));

		Viewing viewing(game);
		Client client(client_end, {.commands = command_codec<Input>(), .replica = &viewing.replica});
		ASSERT_TRUE(client.connect("raw", 1.0));

		// A server played by hand: it takes the Hello and welcomes the client.
		ClientEvent event;
		TransportEvent incoming;
		PeerId peer = NO_PEER;
		EXPECT_FALSE(client.poll(1.0, event));
		while (server_end.poll(1.0, incoming))
			peer = incoming.peer;

		const Message welcome = {.kind = MessageKind::Welcome, .welcome = {.slot = 0, .server_tick = 100}};
		send_message(server_end, peer, welcome, 1.0);
		ASSERT_TRUE(client.poll(1.0, event));
		ASSERT_EQ(event.kind, ClientEventKind::Joined);

		// Two packets: one whose entity section claims tick 0, which is no tick, and a sound one.
		Connection connection(ConnectionDef{}, 1.0);
		CommandQueue queue(command_codec<Input>());
		const auto send = [&](bool junk)
		{
			PacketBuffer buffer;
			serialize::WriteStream stream = packet_writer(buffer);
			const Sequence sequence		  = connection.write_header(stream, 1.0);
			write_command_timing(stream, queue);
			stream.SerializeBits(1u, 1);
			stream.SerializeBits(junk ? NO_TICK : 200u, 32);
			stream.SerializeBits(0u, static_cast<int>(SECTION_COUNT_BITS));
			stream.SerializeBits(0u, static_cast<int>(SECTION_COUNT_BITS));
			stream.Flush();
			server_end.send(peer, written(buffer, stream), Delivery::Unreliable, 1.0);
			return sequence;
		};

		const Sequence junk	 = send(true);
		const Sequence sound = send(false);

		// The client's next packet says which of them it took.
		EXPECT_FALSE(client.poll(1.1, event));
		client.send_command(event.tick + 1, Input{}, 0.0f, 1.1);
		EXPECT_EQ(viewing.replica.latest_tick(), 200u);

		while (server_end.poll(1.2, incoming))
		{
			PacketBuffer buffer;
			serialize::ReadStream stream;
			ASSERT_TRUE(packet_reader(stream, buffer, incoming.data));
			Sequence sequence = 0;
			(void)connection.read_header(stream, sequence, 1.2);
		}

		PacketNotice first;
		PacketNotice second;
		ASSERT_TRUE(connection.take_notice(first));
		ASSERT_TRUE(connection.take_notice(second));
		EXPECT_EQ(first.sequence, junk);
		EXPECT_FALSE(first.delivered) << "to the server it was lost, so whatever mattered in it goes again";
		EXPECT_EQ(second.sequence, sound);
		EXPECT_TRUE(second.delivered);
	}
}
