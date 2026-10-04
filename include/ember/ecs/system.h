#pragma once

#include <ember/ecs/world.h>

#include <array>
#include <string_view>
#include <tuple>
#include <type_traits>

/**
 * Systems are free functions, and their parameters say what they touch:
 *
 *     void motion_system(Sim<const MotionComponent, TransformComponent> movers, const Clock& clock)
 *     {
 *         for (auto [entity, motion, transform] : movers.each())
 *             transform.position += motion.velocity * clock.tick_seconds;
 *     }
 *
 *   Sim<Ts...>        the entities this world simulates that have Ts; a mutable T is one the system writes
 *   View<Ts...>       every entity this world has with Ts, simulated or not
 *   Without<Ts...>    inside either: leaves out entities that have Ts
 *   const T&, T&      the world's T resource, to read or to write
 *   const T*          a resource the world may not have yet, null until it does: a scene's terrain
 *   Commands&         spawns, adds, removes and destroys, landing when the stage ends
 *
 * Sim and View are light wrappers: an EnTT view and the system it belongs to. each() is EnTT's,
 * parallel_each() splits it across every core, and entt() is the view itself, for the rest of its API.
 * What a system reads and writes is known from its parameters alone, so the schedule can show it,
 * check it, and run systems that touch different things at once.
 */
namespace ember::ecs
{
	template <class... Ts> struct Without
	{
	};

	namespace detail
	{
		template <class... Ts> struct TypeList
		{
		};

		template <class Components, class Excluded, class... Ts> struct Split;

		template <class... Cs, class... Es> struct Split<TypeList<Cs...>, TypeList<Es...>>
		{
			using components = TypeList<Cs...>;
			using excluded	 = TypeList<Es...>;
		};

		template <class... Cs, class... Es, class... Ws, class... Rest>
		struct Split<TypeList<Cs...>, TypeList<Es...>, Without<Ws...>, Rest...>
			: Split<TypeList<Cs...>, TypeList<Es..., Ws...>, Rest...>
		{
		};

		template <class... Cs, class... Es, class T, class... Rest>
		struct Split<TypeList<Cs...>, TypeList<Es...>, T, Rest...> : Split<TypeList<Cs..., T>, TypeList<Es...>, Rest...>
		{
		};

		/** The EnTT view a parameter names: Lead, then the components, less those Without leaves out. */
		template <class Lead, class Components, class Excluded> struct ViewOf;

		template <class... Lead, class... Cs, class... Es>
		struct ViewOf<TypeList<Lead...>, TypeList<Cs...>, TypeList<Es...>>
		{
			using type = decltype(std::declval<entt::registry&>().view<Lead..., Cs...>(entt::exclude<Es...>));
		};

		template <class Lead, class... Ts>
		using ViewFor = typename ViewOf<Lead, typename Split<TypeList<>, TypeList<>, Ts...>::components,
										typename Split<TypeList<>, TypeList<>, Ts...>::excluded>::type;
	}

	namespace detail
	{
		/** What Sim and View share: an EnTT view, and the system whose loops it runs. */
		template <bool SIM, class... Ts> class Entities
		{
		public:
			/** The EnTT view underneath: led by Simulated in a Sim. */
			using EnttView = ViewFor<std::conditional_t<SIM, TypeList<const Simulated>, TypeList<>>, Ts...>;

			Entities(EnttView view, SystemContext& context) noexcept : m_view(view), m_context(&context) {}

			/** (entity, components...) for every entity, for a range-for: EnTT's each(). */
			[[nodiscard]] auto each() const noexcept { return m_view.each(); }

			/**
			 * each(), split into ranges of about grain entities that run at once on the job system. fn
			 * changes only its own entity's components: other entities' and the system's resources are
			 * shared by every range, so fn only reads them, and only those the loop does not write. Give fn
			 * a last Commands& to make commands; they land in loop order, as one plain loop's would, and
			 * the system takes Commands& as well.
			 */
			template <class F> void parallel_each(F&& fn, u32 grain = 256) const noexcept;

			/** An entity's components, which it must have. */
			template <class C, class... Cs> [[nodiscard]] decltype(auto) get(Entity entity) const noexcept
			{
				// Inside a split loop, other ranges may be writing their entities' copies of what it writes.
				EMBER_ASSERT(!m_context->loop_writes(entt::type_hash<std::remove_const_t<C>>::value()) &&
							 (!m_context->loop_writes(entt::type_hash<std::remove_const_t<Cs>>::value()) && ...) &&
							 "inside parallel_each, another entity's copy of what the loop writes may be mid-write");
				return m_view.template get<C, Cs...>(entity);
			}

			[[nodiscard]] bool contains(Entity entity) const noexcept { return m_view.contains(entity); }

			/** The EnTT view, for the rest of its API. */
			[[nodiscard]] const EnttView& entt() const noexcept { return m_view; }

		private:
			EnttView m_view;
			SystemContext* m_context;
		};
	}

	/** The entities this world simulates with these components. Mutable ones are what the system writes. */
	template <class... Ts> class Sim final : public detail::Entities<true, Ts...>
	{
	public:
		using detail::Entities<true, Ts...>::Entities;
	};

	/** Every entity this world has with these components, simulated or not. */
	template <class... Ts> class View final : public detail::Entities<false, Ts...>
	{
	public:
		using detail::Entities<false, Ts...>::Entities;
	};

	/** One thing a system touches: a component type or a resource, read or written. */
	struct AccessInfo
	{
		entt::id_type type = 0;
		std::string_view name;
		bool write = false;
	};

	namespace detail
	{
		template <class> inline constexpr bool always_false = false;

		template <class> inline constexpr bool is_view					  = false;
		template <class... Ts> inline constexpr bool is_view<Sim<Ts...>>  = true;
		template <class... Ts> inline constexpr bool is_view<View<Ts...>> = true;

		/** A type's name for the schedule: as EMBER_COMPONENT spells it, or without its namespace. */
		template <class T> [[nodiscard]] constexpr std::string_view type_name() noexcept
		{
			if constexpr (Component<std::remove_const_t<T>>)
			{
				return description_of<std::remove_const_t<T>>.name;
			}
			else
			{
				const std::string_view full = entt::type_name<std::remove_const_t<T>>::value();
				const size_t colon			= full.substr(0, full.find('<')).rfind("::");
				return colon == std::string_view::npos ? full : full.substr(colon + 2);
			}
		}

		template <class T> void note_access(Vector<AccessInfo>& access)
		{
			access.push_back({entt::type_hash<std::remove_const_t<T>>::value(), type_name<T>(), !std::is_const_v<T>});
		}

		/** One entry a type: what a system takes twice, it writes if either one writes it. */
		inline void merge(Vector<AccessInfo>& access)
		{
			for (size_t i = 0; i < access.size(); ++i)
			{
				for (size_t j = access.size() - 1; j > i; --j)
				{
					if (access[j].type != access[i].type)
						continue;

					access[i].write = access[i].write || access[j].write;
					access.erase(access.begin() + static_cast<std::ptrdiff_t>(j));
				}
			}
		}

		// What each phase may take. Each check is its own instantiation, so an error names the component.
		template <Phase phase, bool sim, class C> constexpr bool check_component()
		{
			using T = std::remove_const_t<C>;
			static_assert(Component<T>, "not a component: describe it with EMBER_COMPONENT");

			if constexpr (phase == Phase::Simulate)
			{
				static_assert(!ClientComponent<T>, "Simulate systems never touch Client components: a server has none");
				static_assert(sim || std::is_const_v<C>,
							  "a View in Simulate reads what this world does not simulate: take it const");
			}
			else
			{
				static_assert(!sim, "Sim is for what a world simulates: Input and Present systems take View");
				static_assert(!ServerComponent<T>, "a client never has Server components");
				static_assert(!SimComponent<T> || std::is_const_v<C>,
							  "presentation never writes game state: take it const");
			}
			return true;
		}

		/**
		 * One kind of system parameter: how a run makes it, whether a phase may take it, what it
		 * touches, and the storages it needs before anything runs.
		 */
		template <class P> struct Param
		{
			static_assert(always_false<P>, "a system takes Sim, View, its resources as const T& or T& (const T* for "
										   "one that may not be there), and Commands&");
		};

		/** A Sim or a View: an EnTT view, made afresh for each run, with the system's context. */
		template <class Handle, bool SIM, class... Ts> struct ViewParam
		{
			using Components = typename Split<TypeList<>, TypeList<>, Ts...>::components;
			using Excluded	 = typename Split<TypeList<>, TypeList<>, Ts...>::excluded;

			template <class... Cs, class... Es>
			static typename Handle::EnttView view(entt::registry& registry, TypeList<Cs...>*, TypeList<Es...>*)
			{
				if constexpr (SIM)
					return registry.view<const Simulated, Cs...>(entt::exclude<Es...>);
				else
					return registry.view<Cs...>(entt::exclude<Es...>);
			}

			static Handle make(World& world, SystemContext& context)
			{
				return Handle(view(world.registry, static_cast<Components*>(nullptr), static_cast<Excluded*>(nullptr)),
							  context);
			}

			template <Phase phase, class... Cs> static constexpr bool check_all(TypeList<Cs...>*)
			{
				return (check_component<phase, SIM, Cs>() && ...);
			}

			template <Phase phase> static constexpr bool check()
			{
				return check_all<phase>(static_cast<Components*>(nullptr));
			}

			template <class... Cs> static void note_all(Vector<AccessInfo>& access, TypeList<Cs...>*)
			{
				(note_access<Cs>(access), ...);
			}

			static void note(Vector<AccessInfo>& access) { note_all(access, static_cast<Components*>(nullptr)); }

			// Making the view once makes every storage it reads, the excluded ones too.
			static void prepare(entt::registry& registry) noexcept
			{
				(void)view(registry, static_cast<Components*>(nullptr), static_cast<Excluded*>(nullptr));
			}
		};

		template <class... Ts> struct Param<Sim<Ts...>> : ViewParam<Sim<Ts...>, true, Ts...>
		{
		};

		template <class... Ts> struct Param<View<Ts...>> : ViewParam<View<Ts...>, false, Ts...>
		{
		};

		/** A resource: const T& reads it, T& writes it. */
		template <class T> struct Param<T&>
		{
			using Resource = std::remove_const_t<T>;
			static_assert(!Component<Resource>, "a component is not a resource: reach it through a Sim or a View");
			static_assert(!is_view<Resource>, "take a Sim or a View by value: it is a handle");
			static_assert(!std::is_same_v<Resource, Commands>, "take Commands& to make commands");
			static_assert(!std::is_same_v<Resource, entt::registry> && !std::is_same_v<Resource, World>,
						  "a system takes no registry or world: its parameters say what it touches");

			static T& make(World& world, SystemContext&) { return world.resource<Resource>(); }
			template <Phase> static constexpr bool check() { return true; }
			static void note(Vector<AccessInfo>& access) { note_access<T>(access); }
			static void prepare(entt::registry&) noexcept {}
		};

		/** A resource the world may not have: const T* reads it, and is null while the world has none. */
		template <class T> struct Param<const T*>
		{
			static_assert(!Component<T>, "a component is not a resource: reach it through a Sim or a View");
			static_assert(!is_view<T>, "take a Sim or a View by value: it is a handle");

			static const T* make(World& world, SystemContext&) { return world.registry.ctx().template find<T>(); }
			template <Phase> static constexpr bool check() { return true; }
			static void note(Vector<AccessInfo>& access) { note_access<const T>(access); }
			static void prepare(entt::registry&) noexcept {}
		};

		template <> struct Param<Commands&>
		{
			static Commands& make(World&, SystemContext& context) { return context.commands(); }
			template <Phase> static constexpr bool check() { return true; }
			static void note(Vector<AccessInfo>&) {}
			static void prepare(entt::registry&) noexcept {}
		};

		template <class F> struct FunctionTraits;
		template <class... Ps> struct FunctionTraits<void (*)(Ps...)>
		{
			using params = TypeList<Ps...>;
		};

		/** A function's name, as the compiler spells the template argument: "motion_system". */
		template <auto F> [[nodiscard]] constexpr std::string_view function_name() noexcept
		{
#if defined(_MSC_VER) && !defined(__clang__)
			constexpr std::string_view pretty = __FUNCSIG__; // "...function_name<&game::motion_system>(void) noexcept"
			constexpr size_t start			  = pretty.find("function_name<") + 14;
			constexpr size_t end			  = pretty.rfind(">(void)");
#else
			constexpr std::string_view pretty = __PRETTY_FUNCTION__; // "...[with auto F = game::motion_system; ...]"
			constexpr size_t start			  = pretty.find("F = ") + 4;
			constexpr size_t end			  = pretty.find_first_of(";]", start);
#endif
			std::string_view full = pretty.substr(start, end - start);

			// A whole signature, as MSVC may write it: the name ends where its parameters begin.
			if (!full.empty() && full.back() == ')')
			{
				u32 depth = 0;
				for (size_t i = full.size(); i-- > 0;)
				{
					if (full[i] == ')')
						++depth;
					else if (full[i] == '(' && --depth == 0)
					{
						full = full.substr(0, i);
						break;
					}
				}
			}

			// A template's arguments are not its name: "sync<&Position::value>" is "sync".
			if (const size_t angle = full.find('<'); angle != std::string_view::npos)
				full = full.substr(0, angle);

			if (const size_t space = full.rfind(' '); space != std::string_view::npos)
				full = full.substr(space + 1);
			if (const size_t colon = full.rfind(':'); colon != std::string_view::npos)
				full = full.substr(colon + 1);
			if (!full.empty() && full.front() == '&')
				full = full.substr(1);
			return full;
		}

		/** A function's name, terminated, in storage that lasts as long as the program. */
		template <auto F>
		inline constexpr auto FUNCTION_NAME = []
		{
			constexpr std::string_view name = function_name<F>();
			std::array<char, name.size() + 1> terminated{};
			for (size_t i = 0; i < name.size(); ++i)
				terminated[i] = name[i];
			return terminated;
		}();

		/** What a split loop's fn takes: an entity and the view's components, and perhaps Commands&. */
		template <class F, class Components> struct LoopFn;

		template <class F, class... Cs> struct LoopFn<F, std::tuple<Cs...>>
		{
			static constexpr bool PLAIN	   = std::is_invocable_v<F&, Entity, Cs...>;
			static constexpr bool COMMANDS = std::is_invocable_v<F&, Entity, Cs..., Commands&>;
		};
	}

	namespace detail
	{
		/** The types a view writes: its mutable components. */
		template <class... Cs> constexpr auto written(TypeList<Cs...>*) noexcept
		{
			std::array<entt::id_type, (0 + ... + (std::is_const_v<Cs> ? 0 : 1))> types{};
			size_t at = 0;
			((std::is_const_v<Cs> ? void() : void(types[at++] = entt::type_hash<Cs>::value())), ...);
			return types;
		}

		template <bool SIM, class... Ts>
		template <class F>
		void Entities<SIM, Ts...>::parallel_each(F&& fn, u32 grain) const noexcept
		{
			using Loop = LoopFn<std::remove_reference_t<F>, decltype(m_view.get(std::declval<Entity>()))>;
			static_assert(Loop::PLAIN || Loop::COMMANDS,
						  "parallel_each calls fn(entity, components...) or fn(entity, components..., Commands&)");
			constexpr bool MAKES_COMMANDS = Loop::COMMANDS && !Loop::PLAIN;
			EMBER_ASSERT((!MAKES_COMMANDS || m_context->structural()) &&
						 "a system whose split loop makes commands takes Commands& too");

			static constexpr auto WRITES =
				written(static_cast<typename Split<TypeList<>, TypeList<>, Ts...>::components*>(nullptr));

			const auto* leading = m_view.handle();
			const u32 count		= leading != nullptr ? static_cast<u32>(leading->size()) : 0;

			auto range = [&](u32 begin, u32 end, u32 segment) noexcept
			{
				[[maybe_unused]] Commands commands(*m_context, segment);
				for (u32 i = begin; i < end; ++i)
				{
					// From the back, as EnTT's own loops go: the entities each() gives, in its order.
					const Entity entity = (*leading)[count - 1 - i];
					if (!m_view.contains(entity))
						continue;

					std::apply(
						[&](auto&... components)
						{
							if constexpr (MAKES_COMMANDS)
								fn(entity, components..., commands);
							else
								fn(entity, components...);
						},
						m_view.get(entity));
				}
			};

			using Range = decltype(range);
			m_context->split(
				count, grain, [](u32 begin, u32 end, u32 segment, void* data) noexcept
				{ (*static_cast<Range*>(data))(begin, end, segment); }, &range,
				Span<const entt::id_type>(WRITES.data(), WRITES.size()));
		}
	}

	/** A system as the schedule keeps it: its name, when and where it runs, and what it touches. */
	struct SystemInfo
	{
		std::string_view name; // terminated, so data() serves the job system and the profiler
		Phase phase = Phase::Simulate;
		u8 stage	= 0;
		Where where = Where::Everywhere;
		Vector<AccessInfo> access; // one entry a type
		bool structural = false;   // takes Commands&

		void (*run)(World& world, SystemContext& context)  = nullptr;
		void (*prepare)(entt::registry& registry) noexcept = nullptr; // makes the storages its views read
		const char* (*stage_name)(u8 stage) noexcept	   = nullptr; // when the game's stage enum has names
	};

	/**
	 * What a game is made of: its component types, its prefabs and its systems, in the order the
	 * game's features register them. Every world is made from one, and every machine in a session
	 * registers the same things in the same order: their order is how the wire names them.
	 *
	 *     void movement(Registry& registry)
	 *     {
	 *         registry.components<TransformComponent, MotionComponent>();
	 *         registry.prefabs(CRAWLER, ELITE_CRAWLER);
	 *         registry.simulate<motion_system>(Stage::Move);
	 *     }
	 *
	 * Stages are the game's own enum: Simulate runs them in the order of their values, and the
	 * systems within each in the order they were registered. Give the enum EMBER_ENUM_NAMES and the
	 * schedule shows its names.
	 */
	class Registry final
	{
	public:
		Registry() noexcept = default;

		Registry(const Registry&)			 = delete;
		Registry& operator=(const Registry&) = delete;

		template <Component... Ts> void components() noexcept { (m_components.add<Ts>(), ...); }

		/** Prefabs (prefab.h), and every component they have. Each is a constant that lasts: spawns find it by address.
		 */
		template <class... Defs> void prefabs(Defs&&... definitions) noexcept
		{
			static_assert((std::is_lvalue_reference_v<Defs> && ...),
						  "register prefab constants: inline constexpr auto CRAWLER = ecs::prefab(...)");
			(add_definition(definitions), ...);
		}

		/** A prefab of bytes rather than a definition, as a prefab file makes: its components registered already. */
		PrefabId add_prefab(Prefab prefab) noexcept { return m_prefabs.add(std::move(prefab)); }

		/** A Simulate system: game rules. Everywhere runs it on the server, and on a client for what it predicts. */
		template <auto System, class Stage>
			requires std::is_enum_v<Stage>
		void simulate(Stage stage, Where where = Where::Everywhere) noexcept
		{
			SystemInfo& info = add<Phase::Simulate, System>(static_cast<u8>(stage), where);
			if constexpr (requires { EnumNames<Stage>{}(); })
				info.stage_name = [](u8 value) noexcept { return enum_name(static_cast<Stage>(value)); };
		}

		/** A Present system: a client, every frame. */
		template <auto System> void present() noexcept { (void)add<Phase::Present, System>(0, Where::Client); }

		/** An Input system: a client, every tick, before Simulate. */
		template <auto System> void input() noexcept { (void)add<Phase::Input, System>(0, Where::Client); }

		[[nodiscard]] Span<const SystemInfo> systems() const noexcept
		{
			return Span<const SystemInfo>(m_systems.data(), m_systems.size());
		}

		[[nodiscard]] const Components& component_types() const noexcept { return m_components; }
		[[nodiscard]] const Prefabs& prefab_types() const noexcept { return m_prefabs; }

	private:
		template <Component... Cs> void add_definition(const PrefabDef<Cs...>& definition) noexcept
		{
			components<Cs...>();
			(void)m_prefabs.add(detail::make_prefab(m_components, definition));
		}

		template <Phase phase, auto System> SystemInfo& add(u8 stage, Where where) noexcept
		{
			using params = typename detail::FunctionTraits<decltype(System)>::params;
			return add_with<phase, System>(stage, where, static_cast<params*>(nullptr));
		}

		template <Phase phase, auto System, class... Ps>
		SystemInfo& add_with(u8 stage, Where where, detail::TypeList<Ps...>*) noexcept
		{
			static_assert((detail::Param<Ps>::template check<phase>() && ...));

			const auto& name = detail::FUNCTION_NAME<System>;

			SystemInfo& info = m_systems.emplace_back();
			info.name		 = std::string_view(name.data(), name.size() - 1);
			info.phase		 = phase;
			info.stage		 = stage;
			info.where		 = where;
			info.structural	 = (std::is_same_v<Ps, Commands&> || ...);
			(detail::Param<Ps>::note(info.access), ...);
			detail::merge(info.access);
			info.run = []([[maybe_unused]] World& world, [[maybe_unused]] SystemContext& context)
			{ System(detail::Param<Ps>::make(world, context)...); };
			info.prepare = []([[maybe_unused]] entt::registry& registry) noexcept
			{ (detail::Param<Ps>::prepare(registry), ...); };
			return info;
		}

		Components m_components;
		Prefabs m_prefabs;
		Vector<SystemInfo> m_systems{&memory::heap(MemoryTag::ECS)};
	};
}
