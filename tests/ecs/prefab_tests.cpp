#include <ember/ecs/system.h>

#include <gtest/gtest.h>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace
{
	using namespace ember;
	using namespace ember::ecs;

	struct TransformComponent
	{
		glm::vec2 position = {};
	};
	EMBER_COMPONENT(TransformComponent, Sim);

	struct HealthComponent
	{
		u16 current = 50;
		u16 max		= 50;

		bool operator==(const HealthComponent&) const = default;
	};
	EMBER_COMPONENT(HealthComponent, Sim);

	struct SpriteComponent
	{
		u32 sheet	   = 0;
		glm::vec4 tint = {1.0f, 1.0f, 1.0f, 1.0f};
	};
	EMBER_COMPONENT(SpriteComponent, Client);

	struct GlowComponent
	{
		f32 radius = 4.0f;
	};
	EMBER_COMPONENT(GlowComponent, Client);

	struct BrainComponent
	{
		f32 aggro = 200.0f;
	};
	EMBER_COMPONENT(BrainComponent, Server);

	inline constexpr auto CRAWLER = prefab("enemies/crawler", TransformComponent{},
										   HealthComponent{.current = 30, .max = 30}, SpriteComponent{.sheet = 2});

	inline constexpr auto ELITE_CRAWLER =
		variant(CRAWLER, "enemies/elite_crawler", HealthComponent{.current = 60, .max = 60}, GlowComponent{});

	inline constexpr auto BOSS = variant(ELITE_CRAWLER, "enemies/boss", BrainComponent{.aggro = 900.0f},
										 SpriteComponent{.sheet = 7, .tint = {1.0f, 0.2f, 0.2f, 1.0f}});

	// All of it is known to the compiler.
	static_assert(CRAWLER.name == "enemies/crawler");
	static_assert(std::get<HealthComponent>(CRAWLER.components).current == 30);
	static_assert(std::tuple_size_v<decltype(ELITE_CRAWLER.components)> == 4);
	static_assert(std::get<HealthComponent>(ELITE_CRAWLER.components).max == 60);
	static_assert(std::get<SpriteComponent>(ELITE_CRAWLER.components).sheet == 2, "the base's, untouched");
	static_assert(std::get<GlowComponent>(ELITE_CRAWLER.components).radius == 4.0f);
	static_assert(std::get<HealthComponent>(BOSS.components).current == 60, "a variant of a variant");
	static_assert(std::get<SpriteComponent>(BOSS.components).sheet == 7);
	static_assert(std::is_same_v<std::remove_cvref_t<decltype(std::get<0>(BOSS.components))>, TransformComponent>,
				  "the base's components first, in its order");

	template <class T> [[nodiscard]] T value_of(const Registry& registry, const Prefab& prefab)
	{
		const PrefabComponent* component = prefab.find(registry.component_types().find<T>()->id);
		EXPECT_NE(component, nullptr);
		T value{};
		if (component != nullptr)
			std::memcpy(&value, component->value.data(), sizeof(T));
		return value;
	}

	TEST(Prefab, RegisteringOneRegistersItsComponents)
	{
		Registry registry;
		registry.components<BrainComponent>();
		registry.prefabs(CRAWLER, ELITE_CRAWLER);

		// In the order they were met: the game's own first, then the prefabs' as they came.
		const Components& types = registry.component_types();
		EXPECT_EQ(types.count(), 5u);
		EXPECT_EQ(types.find<BrainComponent>()->id, 0);
		EXPECT_EQ(types.find<TransformComponent>()->id, 1);
		EXPECT_EQ(types.find<GlowComponent>()->id, 4);
	}

	TEST(Prefab, IdsFollowTheOrderOfRegistration)
	{
		Registry registry;
		registry.prefabs(CRAWLER, ELITE_CRAWLER, BOSS);

		const Prefabs& prefabs = registry.prefab_types();
		ASSERT_EQ(prefabs.count(), 3u);
		EXPECT_EQ(prefabs[0].name, "enemies/crawler");
		EXPECT_EQ(prefabs.find(BOSS)->id, 2u);
		EXPECT_EQ(prefabs.find("enemies/elite_crawler"), prefabs.find(ELITE_CRAWLER));
		EXPECT_EQ(prefabs.find("enemies/nothing"), nullptr);
		EXPECT_EQ(prefabs.find(BOSS)->definition, &BOSS);
	}

	TEST(Prefab, ARegisteredPrefabIsItsValuesAsBytes)
	{
		Registry registry;
		registry.prefabs(CRAWLER, ELITE_CRAWLER, BOSS);

		const Prefab& boss = *registry.prefab_types().find(BOSS);
		EXPECT_EQ(boss.components.size(), 5u);
		EXPECT_EQ(value_of<HealthComponent>(registry, boss), (HealthComponent{.current = 60, .max = 60}));
		EXPECT_EQ(value_of<SpriteComponent>(registry, boss).tint, glm::vec4(1.0f, 0.2f, 0.2f, 1.0f));
		EXPECT_EQ(value_of<BrainComponent>(registry, boss).aggro, 900.0f);

		const Prefab& crawler = *registry.prefab_types().find(CRAWLER);
		EXPECT_EQ(value_of<HealthComponent>(registry, crawler).current, 30);
		EXPECT_EQ(crawler.find(registry.component_types().find<GlowComponent>()->id), nullptr);
	}

	TEST(Prefab, APrefabOfBytesRegistersBesideTheRest)
	{
		// What a prefab file will make: no definition, values from wherever.
		Registry registry;
		registry.prefabs(CRAWLER);

		Prefab loaded;
		loaded.name = "enemies/from_a_file";
		const HealthComponent health{.current = 5, .max = 9};
		PrefabComponent& component = loaded.components.emplace_back();
		component.id			   = registry.component_types().find<HealthComponent>()->id;
		component.value.resize(sizeof(health));
		std::memcpy(component.value.data(), &health, sizeof(health));

		EXPECT_EQ(registry.add_prefab(std::move(loaded)), 1u);

		World world(registry);
		const Entity entity = world.spawn(world.prefabs()[1]);
		EXPECT_EQ(world.registry.get<HealthComponent>(entity), health);
		EXPECT_EQ(world.registry.get<PrefabRef>(entity).id, 1u);
	}

	TEST(Prefab, AWorldSpawnsOneByItsDefinition)
	{
		Registry registry;
		registry.prefabs(CRAWLER, ELITE_CRAWLER);
		World world(registry);

		const Entity elite = world.spawn(ELITE_CRAWLER, TransformComponent{.position = {3.0f, 4.0f}});
		EXPECT_EQ(&world.prefab(ELITE_CRAWLER), world.prefabs().find(ELITE_CRAWLER));
		EXPECT_EQ(world.registry.get<PrefabRef>(elite).id, 1u);
		EXPECT_EQ(world.registry.get<TransformComponent>(elite).position, glm::vec2(3.0f, 4.0f));
		EXPECT_EQ(world.registry.get<HealthComponent>(elite).current, 60);
		EXPECT_EQ(world.registry.get<GlowComponent>(elite).radius, 4.0f);
	}
}
