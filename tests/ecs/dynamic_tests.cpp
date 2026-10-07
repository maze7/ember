#include <ember/ecs/system.h>
#include <ember/net/serialize.h>

#include <gtest/gtest.h>

#include <glm/vec2.hpp>

/**
 * Component types declared at run time, as a script's `component` declares them: laid out from their
 * fields, kept in storages of their own, made from prefabs, carried by commands, and across the wire
 * and between two samples like any C++ type.
 */
namespace
{
	using namespace ember;
	using namespace ember::ecs;

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
	EMBER_COMPONENT(Position, Interpolated);

	inline constexpr auto THING = prefab("thing", Position{});

	[[nodiscard]] DynamicComponentDef hops_def()
	{
		DynamicComponentDef def;
		def.name = "Hops";
		def.kind = Kind::Interpolated;
		def.fields.push_back({.name = "reach", .type = FieldType::F32, .value = 40.0});
		def.fields.push_back({.name = "hold", .type = FieldType::U16, .value = 12.0});
		def.fields.push_back({.name = "ready", .type = FieldType::Bool, .value = 1.0});
		def.fields.push_back({.name = "way", .type = FieldType::Vec2, .value = 1.0, .y = 0.0f});
		def.fields.push_back({.name = "hopped", .type = FieldType::I32, .value = -3.0});
		return def;
	}

	TEST(DynamicComponents, AreLaidOutFromTheirFieldsAndStartAtTheirValues)
	{
		Registry registry;
		registry.prefabs(THING);
		const ComponentId id = registry.add_component(hops_def());
		ASSERT_NE(id, NO_COMPONENT);

		const ComponentInfo& info = registry.component_types()[id];
		EXPECT_EQ(info.name, "Hops");
		EXPECT_TRUE(info.dynamic);
		EXPECT_EQ(info.kind, Kind::Interpolated | Kind::Replicated | Kind::Sim);
		ASSERT_EQ(info.fields.size(), 5u);
		EXPECT_EQ(info.fields[0].offset, 0u);  // f32 reach
		EXPECT_EQ(info.fields[1].offset, 4u);  // u16 hold
		EXPECT_EQ(info.fields[2].offset, 6u);  // bool ready
		EXPECT_EQ(info.fields[3].offset, 8u);  // vec2 way, aligned to 4
		EXPECT_EQ(info.fields[4].offset, 16u); // i32 hopped
		EXPECT_EQ(info.size, 24u);
		EXPECT_EQ(info.defaults.size(), 24u);

		EXPECT_EQ(read_field(*info.field("reach"), info.defaults.data()), 40.0);
		EXPECT_EQ(read_field(*info.field("hold"), info.defaults.data()), 12.0);
		EXPECT_EQ(read_field(*info.field("ready"), info.defaults.data()), 1.0);
		EXPECT_EQ(read_field(*info.field("hopped"), info.defaults.data()), -3.0);
		EXPECT_EQ(info.field("nothing"), nullptr);
		EXPECT_EQ(registry.component_types().find("Hops"), &info);
	}

	TEST(DynamicComponents, RefuseWhatCannotBeRegistered)
	{
		Registry registry;
		registry.prefabs(THING);

		DynamicComponentDef taken = hops_def();
		taken.name				  = "Position";
		EXPECT_EQ(registry.add_component(taken), NO_COMPONENT) << "a C++ type has the name";

		DynamicComponentDef twice = hops_def();
		twice.fields.push_back({.name = "reach", .type = FieldType::F32});
		EXPECT_EQ(registry.add_component(twice), NO_COMPONENT) << "a field twice";

		DynamicComponentDef homeless = hops_def();
		homeless.kind				 = Kind::Server | Kind::Client;
		EXPECT_EQ(registry.add_component(homeless), NO_COMPONENT) << "two homes";

		DynamicComponentDef wide = hops_def();
		wide.name				 = "Wide";
		for (u32 i = 0; i < 20; ++i)
			wide.fields.push_back({.name = String("f") + static_cast<char>('a' + i), .type = FieldType::Vec2});
		EXPECT_EQ(registry.add_component(wide), NO_COMPONENT) << "Replicated past 64 bytes";

		wide.kind = Kind::Server;
		EXPECT_NE(registry.add_component(wide), NO_COMPONENT) << "a Server type may be as wide as it likes";

		ASSERT_NE(registry.add_component(hops_def()), NO_COMPONENT);
		EXPECT_EQ(registry.add_component(hops_def()), NO_COMPONENT) << "a name twice";
	}

	TEST(DynamicComponents, LiveInTheirOwnStoragesThroughTheirOperations)
	{
		Registry registry;
		registry.prefabs(THING);
		const ComponentId id = registry.add_component(hops_def());
		ASSERT_NE(id, NO_COMPONENT);

		DynamicComponentDef tag;
		tag.name			  = "Frenzied";
		tag.kind			  = Kind::Server;
		const ComponentId mark = registry.add_component(tag);
		ASSERT_NE(mark, NO_COMPONENT);
		EXPECT_EQ(registry.component_types()[mark].size, 0u);

		World world(registry);
		const ComponentInfo& hops	  = registry.component_types()[id];
		const ComponentInfo& frenzied = registry.component_types()[mark];
		const Entity entity			  = world.spawn(THING);

		EXPECT_EQ(hops.find(hops, world.registry, entity), nullptr);
		hops.emplace(hops, world.registry, entity, hops.defaults.data());
		const void* bytes = hops.find(hops, world.registry, entity);
		ASSERT_NE(bytes, nullptr);
		EXPECT_EQ(read_field(*hops.field("hold"), bytes), 12.0);

		write_field(*hops.field("hold"), hops.get(hops, world.registry, entity), 70000.0);
		EXPECT_EQ(read_field(*hops.field("hold"), bytes), 65535.0) << "held to the field's range";

		// A second value replaces the first; the storage is the one named for the type.
		Vector<u8> other(hops.defaults);
		write_field(*hops.field("reach"), other.data(), 7.0);
		hops.emplace(hops, world.registry, entity, other.data());
		EXPECT_EQ(read_field(*hops.field("reach"), hops.find(hops, world.registry, entity)), 7.0);
		EXPECT_NE(world.registry.storage(hops.type), nullptr);
		EXPECT_EQ(world.registry.storage(hops.type)->size(), 1u);

		hops.remove(hops, world.registry, entity);
		EXPECT_EQ(hops.find(hops, world.registry, entity), nullptr);
		hops.remove(hops, world.registry, entity); // twice is nothing

		EXPECT_EQ(frenzied.find(frenzied, world.registry, entity), nullptr);
		frenzied.emplace(frenzied, world.registry, entity, nullptr);
		EXPECT_NE(frenzied.find(frenzied, world.registry, entity), nullptr);
		frenzied.remove(frenzied, world.registry, entity);
		EXPECT_EQ(frenzied.find(frenzied, world.registry, entity), nullptr);
	}

	TEST(DynamicComponents, ComeWithPrefabsAndCommands)
	{
		Registry registry;
		registry.prefabs(THING);
		const ComponentId id = registry.add_component(hops_def());
		const ComponentInfo& hops = registry.component_types()[id];

		// A prefab of bytes, as a script's prefab is registered: the C++ base's components and the new one.
		Prefab hopper;
		hopper.name = "hopper";
		for (const PrefabComponent& component : registry.prefab_types().find("thing")->components)
			hopper.components.push_back(component);
		PrefabComponent& added = hopper.components.emplace_back();
		added.id			   = id;
		added.value			   = hops.defaults;
		write_field(*hops.field("reach"), added.value.data(), 99.0);
		const PrefabId prefab = registry.add_prefab(std::move(hopper));

		World server(registry, Role::Server);
		const Entity made = server.spawn(registry.prefab_types()[prefab]);
		ASSERT_NE(hops.find(hops, server.registry, made), nullptr);
		EXPECT_EQ(read_field(*hops.field("reach"), hops.find(hops, server.registry, made)), 99.0);
		EXPECT_TRUE(server.registry.all_of<Position>(made));

		// Commands by id carry its bytes, and respect where it lives.
		const Entity bare = server.spawn(THING);
		server.commands().add(bare, id, hops.defaults.data());
		server.apply_commands();
		ASSERT_NE(hops.find(hops, server.registry, bare), nullptr);
		server.commands().remove(bare, id);
		server.apply_commands();
		EXPECT_EQ(hops.find(hops, server.registry, bare), nullptr);

		DynamicComponentDef client_only;
		client_only.name	  = "Glow";
		client_only.kind	  = Kind::Client;
		const ComponentId glow = registry.add_component(client_only);
		World another(registry, Role::Server);
		const Entity dull = another.spawn(THING);
		another.commands().add(dull, glow, nullptr);
		another.apply_commands();
		const ComponentInfo& glow_info = registry.component_types()[glow];
		EXPECT_EQ(glow_info.find(glow_info, another.registry, dull), nullptr) << "a server has no Client components";
	}

	TEST(DynamicComponents, CrossTheWireAndMoveBetweenSamples)
	{
		Registry registry;
		registry.prefabs(THING);
		const ComponentId id = registry.add_component(hops_def());
		const ComponentInfo& hops = registry.component_types()[id];
		ASSERT_NE(hops.write, nullptr);
		ASSERT_NE(hops.read, nullptr);
		ASSERT_NE(hops.interpolate, nullptr);

		Vector<u8> value(hops.defaults);
		write_field(*hops.field("reach"), value.data(), 12.5);
		write_field(*hops.field("hold"), value.data(), 300.0);
		write_field(*hops.field("ready"), value.data(), 0.0);
		write_field(*hops.field("way"), value.data(), -1.0, 0.5f);
		write_field(*hops.field("hopped"), value.data(), -77.0);

		alignas(8) u8 packet[256] = {};
		serialize::WriteStream writer(packet, sizeof(packet));
		ASSERT_TRUE(hops.write(hops, writer, value.data()));
		writer.Flush();

		Vector<u8> back(hops.size, u8{0});
		serialize::ReadStream reader(packet, static_cast<int>((writer.GetBitsProcessed() + 7) / 8));
		ASSERT_TRUE(hops.read(hops, reader, back.data()));
		EXPECT_EQ(std::memcmp(value.data(), back.data(), hops.size), 0);
		EXPECT_EQ(static_cast<u32>(writer.GetBitsProcessed()), 32u + 16u + 1u + 64u + 32u);

		// Halfway: floats and the vector move, the whole numbers and the flag hold the earlier sample.
		Vector<u8> out(hops.size, u8{0});
		hops.interpolate(hops, hops.defaults.data(), value.data(), 0.5f, out.data());
		EXPECT_FLOAT_EQ(static_cast<f32>(read_field(*hops.field("reach"), out.data())), 26.25f);
		EXPECT_EQ(read_field(*hops.field("hold"), out.data()), 12.0);
		EXPECT_EQ(read_field(*hops.field("ready"), out.data()), 1.0);
		EXPECT_EQ(read_field(*hops.field("way"), out.data()), 0.0);
		EXPECT_EQ(read_field(*hops.field("hopped"), out.data()), -3.0);
		hops.interpolate(hops, hops.defaults.data(), value.data(), 1.0f, out.data());
		EXPECT_EQ(std::memcmp(value.data(), out.data(), hops.size), 0);
	}

	TEST(DynamicComponents, ChangeTheFingerprint)
	{
		Registry a;
		a.prefabs(THING);
		Registry b;
		b.prefabs(THING);
		EXPECT_EQ(a.fingerprint(), b.fingerprint());

		ASSERT_NE(a.add_component(hops_def()), NO_COMPONENT);
		EXPECT_NE(a.fingerprint(), b.fingerprint());
		ASSERT_NE(b.add_component(hops_def()), NO_COMPONENT);
		EXPECT_EQ(a.fingerprint(), b.fingerprint());

		Prefab hopper;
		hopper.name = "hopper";
		(void)a.add_prefab(std::move(hopper));
		EXPECT_NE(a.fingerprint(), b.fingerprint());
	}
}
