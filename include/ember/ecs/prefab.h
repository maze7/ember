#pragma once

#include <ember/ecs/components.h>

#include <new>
#include <tuple>

/**
 * Prefabs: entities as a designer writes them, every component they start with at its value,
 * wherever each one lives. A world makes the components that live in it and leaves the rest: a
 * server its Sim and Server ones, a client its Sim and Client ones, a standalone game all of them.
 *
 * Code writes them as constants, checked by the compiler, beside the components they use:
 *
 *     inline constexpr auto CRAWLER = ecs::prefab("crawler",
 *         TransformComponent{},
 *         HealthComponent{.current = 30, .max = 30},
 *         SpriteComponent{.sheet = 2});
 *
 *     // A variant: its base with some values changed, and maybe more components.
 *     inline constexpr auto ELITE_CRAWLER = ecs::variant(CRAWLER, "elite_crawler",
 *         HealthComponent{.current = 60, .max = 60},
 *         GlowComponent{});
 *
 * A game registers them, which registers their components too, and spawns them by name in code:
 *
 *     registry.prefabs(CRAWLER, ELITE_CRAWLER);
 *     commands.spawn(CRAWLER, TransformComponent{.position = at});
 *
 * A registered prefab is a Prefab, its values as bytes: what worlds, tools and the net layer make
 * entities from, and what a prefab file will be read into.
 */
namespace ember::ecs
{
	/** A prefab's place in its registry: the order they were registered in, the same on every machine. */
	using PrefabId = u32;

	/** A prefab as code writes it: a name and its components' values, all known at compile time. */
	template <Component... Cs> struct PrefabDef
	{
		std::string_view name;
		std::tuple<Cs...> components;
	};

	namespace detail
	{
		template <class T, class... Ts> inline constexpr bool one_of = (std::is_same_v<T, Ts> || ...);

		template <class... Ts> struct Distinct : std::true_type
		{
		};

		template <class T, class... Ts>
		struct Distinct<T, Ts...> : std::bool_constant<!one_of<T, Ts...> && Distinct<Ts...>::value>
		{
		};

		/** A base's value, or the override's when there is one of its type. */
		template <class B, class... Os> constexpr B pick(B value, const Os&... overrides) noexcept
		{
			(
				[&](const auto& override)
				{
					if constexpr (std::is_same_v<B, std::remove_cvref_t<decltype(override)>>)
						value = override;
				}(overrides),
				...);
			return value;
		}

		/** An override as a tuple of one, or of none when the base has its type already. */
		template <class... Bs, class O> constexpr auto if_new(const O& override) noexcept
		{
			if constexpr (one_of<O, Bs...>)
				return std::tuple<>{};
			else
				return std::tuple<O>{override};
		}
	}

	/** A prefab: a name, and the components an entity made from it starts with, each type once. */
	template <Component... Cs>
	constexpr PrefabDef<Cs...> prefab(std::string_view name, const Cs&... components) noexcept
	{
		static_assert(detail::Distinct<Cs...>::value, "a prefab has each component once");
		return {name, std::tuple<Cs...>{components...}};
	}

	/** A prefab made from another: the base's components, these values over them, and any new ones after. */
	template <Component... Bs, Component... Os>
	constexpr auto variant(const PrefabDef<Bs...>& base, std::string_view name, const Os&... overrides) noexcept
	{
		static_assert(detail::Distinct<Os...>::value, "a variant changes each component once");

		const auto merged =
			std::apply([&](const Bs&... values) { return std::tuple<Bs...>{detail::pick(values, overrides...)...}; },
					   base.components);
		const auto all = std::tuple_cat(merged, detail::if_new<Bs...>(overrides)...);
		return std::apply([&](const auto&... values) { return prefab(name, values...); }, all);
	}

	/** One component of a prefab: which, and its whole value. */
	struct PrefabComponent
	{
		ComponentId id = 0;
		Vector<u8> value;
	};

	/** A registered prefab: its components as bytes, in the order it lists them. */
	struct Prefab
	{
		String name;
		PrefabId id = 0;
		Vector<PrefabComponent> components;
		const void* definition = nullptr; // the PrefabDef it came from; null for one read from a file

		/** Its value for a component, or null when it has none. */
		[[nodiscard]] const PrefabComponent* find(ComponentId component) const noexcept;
	};

	/**
	 * Every prefab a game has, numbered in the order it registered them, which is the same on every
	 * machine that runs the same build.
	 */
	class Prefabs final
	{
	public:
		Prefabs() noexcept = default;

		Prefabs(const Prefabs&)			   = delete;
		Prefabs& operator=(const Prefabs&) = delete;

		/** Adds a prefab, its components registered already; its id. A name or definition twice is a mistake. */
		PrefabId add(Prefab prefab) noexcept;

		[[nodiscard]] const Prefab* find(StringView name) const noexcept;

		/** The prefab registered from a PrefabDef; null when it never was. */
		template <class... Cs> [[nodiscard]] const Prefab* find(const PrefabDef<Cs...>& definition) const noexcept
		{
			return find_definition(&definition);
		}

		[[nodiscard]] const Prefab& operator[](PrefabId id) const noexcept { return m_prefabs[id]; }
		[[nodiscard]] u32 count() const noexcept { return static_cast<u32>(m_prefabs.size()); }

	private:
		[[nodiscard]] const Prefab* find_definition(const void* definition) const noexcept;

		Vector<Prefab> m_prefabs{&memory::heap(MemoryTag::ECS)};
		HashMap<const void*, PrefabId> m_by_definition{&memory::heap(MemoryTag::ECS)};
	};

	namespace detail
	{
		/** A registered prefab's bytes, from its definition. */
		template <class... Cs>
		[[nodiscard]] Prefab make_prefab(const Components& components, const PrefabDef<Cs...>& def)
		{
			Prefab prefab;
			prefab.name		  = String(def.name);
			prefab.definition = &def;
			std::apply(
				[&](const Cs&... values)
				{
					(
						[&](const auto& value)
						{
							using C					   = std::remove_cvref_t<decltype(value)>;
							// A copy over zeroed bytes: the fields, and none of the source's padding.
							PrefabComponent& component = prefab.components.emplace_back();
							component.id			   = components.find<C>()->id;
							component.value.assign(sizeof(C), 0);
							new (component.value.data()) C(value);
						}(values),
						...);
				},
				def.components);
			return prefab;
		}
	}
}
