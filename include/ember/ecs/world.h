#pragma once

#include <ember/ecs/prefab.h>
#include <ember/memory/unique.h>

#include <semaphore>

namespace ember::jobs
{
	class Counter;
}

namespace ember::ecs
{
	/** Which side of a session a world is. A host runs a server and a client; single player, one standalone. */
	enum class Role : u8
	{
		Standalone, // no session: every component and every system, and it simulates everything
		Server,		// the authority: it simulates everything, and presents nothing
		Client,		// a player's machine: it predicts what it owns, and presents everything
		Count
	};

	/** Whether a world of this role has components of this kind: a server no Client ones, a client no Server ones. */
	[[nodiscard]] constexpr bool lives_in(Kind kind, Role role) noexcept
	{
		switch (role)
		{
			case Role::Server:
				return !has_any(kind, Kind::Client);
			case Role::Client:
				return !has_any(kind, Kind::Server);
			default:
				return true;
		}
	}

	/** The parts of a frame a world runs systems in. */
	enum class Phase : u8
	{
		Input,	  // a client: devices into what the player wants
		Simulate, // every tick: the game's rules, stage by stage
		Present,  // a client, every frame: what the player sees and hears
		Count
	};

	/**
	 * Where a Simulate system runs. Everywhere is the server, and a client for what it predicts. A
	 * standalone world runs them all.
	 */
	enum class Where : u8
	{
		Everywhere,
		Server,
		Client,
		Count
	};

	/**
	 * How a world runs each stage's systems. Every mode makes the same world from systems that keep to
	 * their parameters; they differ in how many cores they use, and in how a mistake shows.
	 */
	enum class RunMode : u8
	{
		Serial, // one at a time in registration order, on the caller: a dedicated server, a match per core
		Jobs,	// at once where the stage's graph allows, on the job system, split loops across the workers:
			  // from main or a job, or from a thread of its own, which sleeps while a job runs the phase
		Shuffled, // for tests: one at a time in random orders the graph allows, split loops' ranges too
		Count
	};

	/** What this world advances: everything on the server; on a client, what it predicts. */
	struct Simulated
	{
	};

	/** The prefab an entity was made from: the same id on every machine. */
	struct PrefabRef
	{
		PrefabId id = 0;
	};

	/** An entity a system asked for. It exists from the end of the stage; until then commands can still add to it. */
	struct Spawned
	{
		u32 segment = 0; // the stretch of its system's commands that asked for it
		u32 index	= 0; // which of that stretch's spawns it is
	};

	class World;
	class SystemContext;
	class Registry;
	struct SystemInfo;

	namespace detail
	{
		using ApplyFn = void (*)(World& world, SystemContext& context, const u8* payload) noexcept;

		/** One range of a split loop: items [begin, end), whose commands go to segment. */
		using RangeFn = void (*)(u32 begin, u32 end, u32 segment, void* data) noexcept;

		/** Where a command lands: an entity, or one its system spawned that the stage has not made yet. */
		struct Target
		{
			Entity entity = NO_ENTITY;
			Spawned spawned;
		};

		/**
		 * A stretch of a system's commands, as bytes: nothing allocated per command. What the system's
		 * own code makes between two split loops is one stretch; each range of a split loop is another.
		 */
		struct CommandStream
		{
			Vector<u8> bytes;
			Vector<Entity> spawned; // filled as the stretch applies, in the order its spawns were asked for
			u32 spawns = 0;			// handed out while the system runs
		};

		struct SpawnPayload
		{
			const Prefab* prefab = nullptr;
			Spawned spawned;
			bool simulated = false;
		};

		template <class T> struct AddPayload
		{
			Target target;
			T value;
		};

		struct RemovePayload
		{
			Target target;
		};

		/** A component by id, its bytes following the payload: what a script or a tool adds. */
		struct RawPayload
		{
			Target target;
			ComponentId component = 0;
		};

		struct DestroyPayload
		{
			Entity entity = NO_ENTITY;
		};
	}

	/**
	 * Structural changes: spawns, adds, removes and destroys. They wait for the end of the stage, then
	 * land in the order the systems made them, as if the systems had run one after another. A command
	 * for an entity that is gone by then does nothing.
	 */
	class Commands final
	{
	public:
		/** The system's own commands; or, given a segment, one range of a split loop's. */
		explicit Commands(SystemContext& context, u32 segment = OWN) noexcept : m_context(&context), m_segment(segment)
		{
		}

		/**
		 * A prefab's entity, with these components set over the prefab's or added to them. Components
		 * that do not live in this world are left out, so shared code may name them all.
		 */
		template <Component... Ts> Spawned spawn(const Prefab& prefab, const Ts&... overrides) noexcept;

		template <Component... Cs, Component... Ts>
		Spawned spawn(const PrefabDef<Cs...>& prefab, const Ts&... overrides) noexcept;

		template <Component T> void add(Entity entity, const T& value) noexcept
		{
			if (lives_here<T>())
				push(stream(), &apply_add<T>,
					 detail::AddPayload<T>{.target = {.entity = entity, .spawned = {}}, .value = value});
		}

		template <Component T> void add(Spawned spawned, const T& value) noexcept
		{
			if (lives_here<T>())
				push(stream(), &apply_add<T>,
					 detail::AddPayload<T>{.target = {.entity = NO_ENTITY, .spawned = spawned}, .value = value});
		}

		template <Component T> void remove(Entity entity) noexcept
		{
			push(stream(), &apply_remove<T>, detail::RemovePayload{.target = {.entity = entity, .spawned = {}}});
		}

		void destroy(Entity entity) noexcept;

		/**
		 * A component by its id rather than its type, as a script or a tool adds one: `value` is the
		 * component's bytes, as many as its type has. Nothing when the type does not live in this world,
		 * as the typed add leaves it out.
		 */
		void add(Entity entity, ComponentId component, const void* value) noexcept;
		void add(Spawned spawned, ComponentId component, const void* value) noexcept;
		void remove(Entity entity, ComponentId component) noexcept;

	private:
		static constexpr u32 OWN = ~u32{0};

		template <class Payload>
		static void push(detail::CommandStream& into, detail::ApplyFn apply, const Payload& payload) noexcept
		{
			static_assert(std::is_trivially_copyable_v<Payload>);
			const size_t at = into.bytes.size();
			const u32 size	= sizeof(Payload);
			into.bytes.resize(at + sizeof(apply) + sizeof(size) + sizeof(Payload));
			std::memcpy(into.bytes.data() + at, &apply, sizeof(apply));
			std::memcpy(into.bytes.data() + at + sizeof(apply), &size, sizeof(size));
			std::memcpy(into.bytes.data() + at + sizeof(apply) + sizeof(size), &payload, sizeof(Payload));
		}

		/** A payload with a component's bytes after it, which are not part of it: an add by id. */
		static void push_raw(detail::CommandStream& into, detail::ApplyFn apply, const detail::RawPayload& payload,
							 const void* bytes, u32 size) noexcept;

		/** Whether T lives in this world: a component that does not is left out, as the world would. */
		template <Component T> [[nodiscard]] bool lives_here() const noexcept;

		/** The stretch these commands go to now. */
		[[nodiscard]] u32 segment() noexcept;
		[[nodiscard]] detail::CommandStream& stream() noexcept;

		template <class T> static void apply_add(World& world, SystemContext& context, const u8* payload) noexcept;
		template <class T> static void apply_remove(World& world, SystemContext& context, const u8* payload) noexcept;

		SystemContext* m_context;
		u32 m_segment;
	};

	/**
	 * One system in one world: the commands it made this stage, stretch by stretch in the order it made
	 * them, and the split loops it runs.
	 */
	class SystemContext final
	{
	public:
		/** system: the one whose commands these are, or null for commands from outside any system. */
		SystemContext(World& world, const SystemInfo* system) noexcept;

		SystemContext(const SystemContext&)			   = delete;
		SystemContext& operator=(const SystemContext&) = delete;

		[[nodiscard]] World& world() const noexcept { return m_world; }
		[[nodiscard]] Commands& commands() noexcept { return m_commands; }

		/** Whether what it spawns is Simulated, as what a Simulate system spawns is. */
		[[nodiscard]] bool simulates() const noexcept { return m_simulates; }

		/** Whether its system takes Commands&, which a split loop that makes commands needs. */
		[[nodiscard]] bool structural() const noexcept { return m_structural; }

		/** True while a split loop's ranges run: then only the loop's own Commands may make commands. */
		[[nodiscard]] bool splitting() const noexcept { return m_splitting; }

		/** Whether a split loop running now writes this type, which its ranges must not read for others. */
		[[nodiscard]] bool loop_writes(entt::id_type type) const noexcept;

		/** The stretch the system's own commands go to: the last one, or a first one. */
		[[nodiscard]] u32 own_segment() noexcept;
		[[nodiscard]] detail::CommandStream& segment(u32 index) noexcept { return m_segments[index]; }

		/**
		 * Runs fn over [0, count) in ranges of about grain items, each with a stretch of commands of its
		 * own after everything the system made before: in order, at once on the job system or in a
		 * random order, as the world's mode says. writes: the types the loop writes.
		 */
		void split(u32 count, u32 grain, detail::RangeFn fn, void* data, Span<const entt::id_type> writes) noexcept;

		/** An entity a command asked for: one it names, or one a spawn of this system made. */
		[[nodiscard]] Entity resolve(const detail::Target& target) noexcept;

		/** Applies every command in order, then forgets them. The world calls this when the stage ends. */
		void apply() noexcept;

	private:
		[[nodiscard]] u32 claim(u32 count) noexcept;

		World& m_world;
		Commands m_commands;
		const char* m_name;
		Vector<detail::CommandStream> m_segments; // kept from stage to stage, so their memory is reused
		u32 m_used = 0;							  // segments this stage has used
		Span<const entt::id_type> m_loop_writes;  // while a split loop runs
		bool m_splitting = false;
		bool m_simulates;
		bool m_structural;
	};

	/**
	 * One machine's game: a standalone game's, the server's, or a client's. Its own entities, resources
	 * and copy of the rules: the systems the game registered, run phase by phase and stage by stage.
	 */
	class World final
	{
	public:
		/** game must outlive the world, and is complete: nothing registers once a world is made from it. */
		explicit World(const Registry& game, Role role = Role::Standalone) noexcept;
		~World() noexcept;

		World(const World&)			   = delete;
		World& operator=(const World&) = delete;

		[[nodiscard]] Role role() const noexcept { return m_role; }
		[[nodiscard]] const Components& components() const noexcept;
		[[nodiscard]] const Prefabs& prefabs() const noexcept;

		/** A registered prefab, from its definition. */
		template <class... Cs> [[nodiscard]] const Prefab& prefab(const PrefabDef<Cs...>& definition) const noexcept
		{
			const Prefab* found = prefabs().find(definition);
			EMBER_ASSERT(found != nullptr && "register the prefab first: registry.prefabs(...)");
			return *found;
		}

		/**
		 * A prefab's entity, now, with these components set over the prefab's or added to them: for
		 * loading a level, tools and tests. Systems spawn with Commands.
		 */
		template <Component... Ts> Entity spawn(const Prefab& prefab, const Ts&... overrides) noexcept
		{
			const Entity entity = create(prefab, m_role != Role::Client);
			(set<Ts>(entity, overrides), ...);
			return entity;
		}

		template <Component... Cs, Component... Ts>
		Entity spawn(const PrefabDef<Cs...>& definition, const Ts&... overrides) noexcept
		{
			return spawn(prefab(definition), overrides...);
		}

		/** Gives an entity a component, or a new value for one it has; nothing when T does not live here. */
		template <Component T> void set(Entity entity, const T& value) noexcept
		{
			if (lives_in(kind_of<T>, m_role))
				registry.emplace_or_replace<T>(entity, value);
		}

		/** A resource: one value of its type for the whole world, which systems take as const T& or T&. */
		template <class T, class... Args> T& add_resource(Args&&... args)
		{
			return registry.ctx().emplace<T>(std::forward<Args>(args)...);
		}

		template <class T> [[nodiscard]] T& resource() noexcept
		{
			EMBER_ASSERT(registry.ctx().contains<T>() && "add the resource before a system takes it");
			return registry.ctx().get<T>();
		}

		/** How run() runs each stage's systems: Serial until set. seed picks Shuffled's orders. */
		void set_mode(RunMode mode, u64 seed = 0) noexcept
		{
			m_mode	 = mode;
			m_random = seed;
		}

		[[nodiscard]] RunMode mode() const noexcept { return m_mode; }

		/**
		 * Runs a phase's systems for this world's role, stage by stage. Within a stage a system waits
		 * for an earlier one only when they touch the same thing and either writes it, so every mode
		 * gives what running them one at a time in registration order gives. The stage's commands land
		 * when its last system returns.
		 *
		 * One thread at a time runs a world. In Jobs mode a thread outside the scheduler, a server's own,
		 * hands the phase to a job and sleeps until it is done: between runs the world is that thread's.
		 */
		void run(Phase phase) noexcept;

		/** The prefab's components that live here: all of them standalone, less the Client ones on a server, less the
		 * Server ones on a client. */
		void instantiate(Entity entity, const Prefab& prefab) noexcept;

		/** A prefab's entity, with what lives here, its PrefabRef, and Simulated when asked. */
		Entity create(const Prefab& prefab, bool simulated) noexcept;

		/** Commands from outside any system: tools, tests, the net layer. They land at apply_commands(). */
		[[nodiscard]] Commands& commands() noexcept { return m_outside.commands(); }
		void apply_commands() noexcept { m_outside.apply(); }

		/**
		 * The schedule this world runs, for a debug panel: each stage's systems in registration order,
		 * what they touch, and whom each waits for and why.
		 */
		[[nodiscard]] String describe(Phase phase) const;

		entt::registry registry;

	private:
		friend class SystemContext;

		struct Stage;

		/** One system of a stage, as the stage runs it. */
		struct Node
		{
			const SystemInfo* system = nullptr;
			Unique<SystemContext> context;
			Vector<u32> next;  // the systems that wait for this one
			Vector<u32> after; // the systems this one waits for
			u32 waits_for = 0;
			u32 waiting	  = 0; // counted down while the stage runs
			World* world  = nullptr;
			Stage* stage  = nullptr;
		};

		/** One stage's systems for this world, in registration order, with the order their access forces on them. */
		struct Stage
		{
			u8 index = 0;
			Vector<Node> nodes;
			jobs::Counter* done = nullptr; // what a Jobs run of the stage waits on
		};

		static void plan(Stage& stage) noexcept;
		void run_stages(Phase phase) noexcept;
		void run_as_job(Phase phase) noexcept;
		static void run_node(void* data) noexcept;
		static void run_inline(Node& node) noexcept;
		void run_jobs(Stage& stage) noexcept;
		void run_shuffled(Stage& stage) noexcept;

		/** The next number for Shuffled's orders. */
		[[nodiscard]] u64 random() noexcept;

		Role m_role;
		const Registry& m_registry;
		RunMode m_mode = RunMode::Serial;
		u64 m_random   = 0;
		SystemContext m_outside;
		Vector<Stage> m_stages[static_cast<u32>(Phase::Count)];

		// A phase handed to a job by a thread outside the scheduler, and how the job hands the world back.
		Phase m_handed = Phase::Count;
		std::binary_semaphore m_handed_back{0};
	};

	template <Component... Ts> Spawned Commands::spawn(const Prefab& prefab, const Ts&... overrides) noexcept
	{
		const u32 at				= segment();
		detail::CommandStream& into = m_context->segment(at);
		const Spawned spawned{.segment = at, .index = into.spawns++};

		push(
			into,
			+[](World& world, SystemContext& context, const u8* bytes) noexcept
			{
				detail::SpawnPayload payload;
				std::memcpy(&payload, bytes, sizeof(payload));

				// Spawns land in the order they were asked for, so a Spawned's index is its place.
				detail::CommandStream& from = context.segment(payload.spawned.segment);
				EMBER_ASSERT(from.spawned.size() == payload.spawned.index);

				from.spawned.push_back(world.create(*payload.prefab, payload.simulated));
			},
			detail::SpawnPayload{.prefab = &prefab, .spawned = spawned, .simulated = m_context->simulates()});

		(add(spawned, overrides), ...);
		return spawned;
	}

	template <Component T> bool Commands::lives_here() const noexcept
	{
		return lives_in(kind_of<T>, m_context->world().role());
	}

	template <Component... Cs, Component... Ts>
	Spawned Commands::spawn(const PrefabDef<Cs...>& prefab, const Ts&... overrides) noexcept
	{
		return spawn(m_context->world().prefab(prefab), overrides...);
	}

	template <class T> void Commands::apply_add(World& world, SystemContext& context, const u8* bytes) noexcept
	{
		detail::AddPayload<T> payload;
		std::memcpy(&payload, bytes, sizeof(payload));

		const Entity entity = context.resolve(payload.target);
		if (world.registry.valid(entity))
			world.set(entity, payload.value);
	}

	template <class T> void Commands::apply_remove(World& world, SystemContext& context, const u8* bytes) noexcept
	{
		detail::RemovePayload payload;
		std::memcpy(&payload, bytes, sizeof(payload));

		const Entity entity = context.resolve(payload.target);
		if (world.registry.valid(entity))
			world.registry.remove<T>(entity);
	}
}
