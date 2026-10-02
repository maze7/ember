#include <ember/ecs/components.h>

#include <gtest/gtest.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace
{
	using namespace ember;
	using namespace ember::ecs;

	enum class Mood : u8
	{
		Calm,
		Angry,
	};

	struct PositionComponent
	{
		glm::vec2 position = {};

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_float(stream, position.x);
			serialize_float(stream, position.y);
			return true;
		}
	};
	EMBER_COMPONENT(PositionComponent, Interpolated | Predicted);

	struct HealthComponent
	{
		u16 current = 50;
		u16 max		= 50;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_int(stream, current, 0, 1000);
			serialize_int(stream, max, 1, 1000);
			return true;
		}
	};
	EMBER_COMPONENT(HealthComponent, Replicated);

	/** Every kind of field the default interpolation meets. */
	struct BodyComponent
	{
		struct Limb
		{
			f32 reach = 0.0f;
			u8 joints = 0;
		};

		glm::vec2 position = {};
		f32 height		   = 0.0f;
		f64 mass		   = 0.0;
		i32 frame		   = 0;
		Mood mood		   = Mood::Calm;
		bool grounded	   = false;
		glm::ivec2 cell	   = {};
		glm::quat facing   = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		Limb arm;

		template <class Stream> bool serialize(Stream&) { return true; }
	};
	EMBER_COMPONENT(BodyComponent, Interpolated);

	/** An angle that wraps: drawn the short way round, as its own interpolate() says. */
	struct AimComponent
	{
		f32 angle = 0.0f;

		static AimComponent interpolate(const AimComponent& from, const AimComponent& to, f32 t)
		{
			f32 delta = to.angle - from.angle;
			if (delta > 180.0f)
				delta -= 360.0f;
			if (delta < -180.0f)
				delta += 360.0f;
			return {from.angle + delta * t};
		}

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_compressed_float(stream, angle, 0.0f, 360.0f, 0.5f);
			return true;
		}
	};
	EMBER_COMPONENT(AimComponent, Interpolated);

	struct BrainComponent
	{
		Mood mood = Mood::Calm;
		f32 aggro = 200.0f;
	};
	EMBER_COMPONENT(BrainComponent, Server);

	struct SpriteComponent
	{
		u32 sheet	   = 0;
		glm::vec4 tint = {1.0f, 1.0f, 1.0f, 1.0f};
	};
	EMBER_COMPONENT(SpriteComponent, Client);

	/** A Replicated tag: having it is all it says, so it needs no serialize(). */
	struct StunnedComponent
	{
	};
	EMBER_COMPONENT(StunnedComponent, Replicated);

	struct NotAComponent
	{
		i32 x = 0;
	};

	template <class T> T interpolated(const Components& types, const T& from, const T& to, f32 t)
	{
		T out{};
		types.find<T>()->interpolate(&from, &to, t, &out);
		return out;
	}

	TEST(Component, KindsImplyWhatTheyMean)
	{
		static_assert(kind_of<PositionComponent> ==
					  (Kind::Sim | Kind::Replicated | Kind::Interpolated | Kind::Predicted));
		static_assert(kind_of<HealthComponent> == (Kind::Sim | Kind::Replicated));
		static_assert(kind_of<BrainComponent> == Kind::Server);
		static_assert(SimComponent<PositionComponent> && ReplicatedComponent<PositionComponent>);
		static_assert(ServerComponent<BrainComponent> && !SimComponent<BrainComponent>);
		static_assert(ClientComponent<SpriteComponent>);
		static_assert(!Component<NotAComponent>);
		static_assert(description_of<HealthComponent>.name == "HealthComponent");

		static_assert(one_home(Kind::Sim) && one_home(Kind::Client));
		static_assert(!one_home(Kind::None) && !one_home(Kind::Sim | Kind::Server));
		static_assert(normalize(Kind::OwnerOnly) == (Kind::Sim | Kind::Replicated | Kind::OwnerOnly));
	}

	TEST(Component, RegistersOnceInOrder)
	{
		Components types;
		EXPECT_EQ(types.add<HealthComponent>(), 0);
		EXPECT_EQ(types.add<SpriteComponent>(), 1);
		EXPECT_EQ(types.add<HealthComponent>(), 0);
		EXPECT_EQ(types.count(), 2u);

		const ComponentInfo& health = *types.find<HealthComponent>();
		EXPECT_EQ(health.name, "HealthComponent");
		EXPECT_EQ(types.find("SpriteComponent"), types.find<SpriteComponent>());
		EXPECT_EQ(types.find("Nothing"), nullptr);
		EXPECT_EQ(types.find<BrainComponent>(), nullptr);

		HealthComponent defaults;
		std::memcpy(&defaults, health.defaults.data(), sizeof(defaults));
		EXPECT_EQ(defaults.current, 50);
	}

	TEST(Component, OnlyReplicatedCrossTheWireAndOnlyInterpolatedMove)
	{
		Components types;
		types.add<HealthComponent>();
		types.add<SpriteComponent>();
		types.add<PositionComponent>();

		EXPECT_NE(types.find<HealthComponent>()->write, nullptr);
		EXPECT_EQ(types.find<HealthComponent>()->interpolate, nullptr);
		EXPECT_EQ(types.find<SpriteComponent>()->write, nullptr);
		EXPECT_NE(types.find<PositionComponent>()->interpolate, nullptr);
	}

	TEST(Component, CrossesTheWireWithItsOwnSerialize)
	{
		Components types;
		types.add<HealthComponent>();
		types.add<StunnedComponent>();
		const ComponentInfo& info = *types.find<HealthComponent>();

		net::PacketBuffer buffer;
		serialize::WriteStream writer = net::packet_writer(buffer);
		const HealthComponent sent{.current = 17, .max = 999};
		ASSERT_TRUE(info.write(writer, &sent));
		ASSERT_TRUE(types.find<StunnedComponent>()->write(writer, nullptr));
		writer.Flush();
		EXPECT_EQ(writer.GetBitsProcessed(), 20); // two ranges of 10 bits, and a tag of none

		serialize::ReadStream reader(buffer.bytes.data(), static_cast<int>(writer.GetBytesProcessed()));
		HealthComponent got;
		ASSERT_TRUE(info.read(reader, &got));
		EXPECT_EQ(got.current, 17);
		EXPECT_EQ(got.max, 999);
	}

	TEST(Component, BadBytesFailTheRead)
	{
		Components types;
		types.add<HealthComponent>();

		// 1023 for current: past its range.
		net::PacketBuffer buffer;
		serialize::WriteStream writer = net::packet_writer(buffer);
		u32 bad						  = 1023;
		writer.SerializeBits(bad, 10);
		writer.SerializeBits(bad, 10);
		writer.Flush();

		serialize::ReadStream reader(buffer.bytes.data(), static_cast<int>(writer.GetBytesProcessed()));
		HealthComponent got{.current = 5, .max = 5};
		EXPECT_FALSE(types.find<HealthComponent>()->read(reader, &got));
		EXPECT_EQ(got.current, 5);
	}

	TEST(Component, InterpolatesFieldByField)
	{
		Components types;
		types.add<BodyComponent>();

		BodyComponent from;
		from.position = {0.0f, 10.0f};
		from.height	  = 1.0f;
		from.mass	  = 2.0;
		from.frame	  = 3;
		from.mood	  = Mood::Calm;
		from.cell	  = {1, 1};
		from.arm	  = {.reach = 1.0f, .joints = 2};

		BodyComponent to;
		to.position = {10.0f, 20.0f};
		to.height	= 3.0f;
		to.mass		= 4.0;
		to.frame	= 9;
		to.mood		= Mood::Angry;
		to.grounded = true;
		to.cell		= {5, 5};
		to.facing	= glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		to.arm		= {.reach = 3.0f, .joints = 4};

		const BodyComponent half = interpolated(types, from, to, 0.5f);
		EXPECT_FLOAT_EQ(half.position.x, 5.0f);
		EXPECT_FLOAT_EQ(half.position.y, 15.0f);
		EXPECT_FLOAT_EQ(half.height, 2.0f);
		EXPECT_DOUBLE_EQ(half.mass, 3.0);
		EXPECT_FLOAT_EQ(half.arm.reach, 2.0f);
		EXPECT_FLOAT_EQ(glm::degrees(glm::angle(half.facing)), 45.0f);

		// What cannot move holds the earlier sample until the later one's tick.
		EXPECT_EQ(half.frame, 3);
		EXPECT_EQ(half.mood, Mood::Calm);
		EXPECT_FALSE(half.grounded);
		EXPECT_EQ(half.cell, glm::ivec2(1, 1));
		EXPECT_EQ(half.arm.joints, 2);

		const BodyComponent end = interpolated(types, from, to, 1.0f);
		EXPECT_EQ(end.frame, 9);
		EXPECT_EQ(end.mood, Mood::Angry);
		EXPECT_TRUE(end.grounded);
		EXPECT_EQ(end.arm.joints, 4);
	}

	TEST(Component, InterpolatesWithItsOwnWhenItHasOne)
	{
		Components types;
		types.add<AimComponent>();

		const AimComponent drawn = interpolated(types, AimComponent{350.0f}, AimComponent{10.0f}, 0.5f);
		EXPECT_FLOAT_EQ(drawn.angle, 360.0f); // through north, not back round through south
	}
}
