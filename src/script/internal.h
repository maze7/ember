#pragma once

#include <ember/anim/components.h>
#include <ember/script/binding.h>
#include <ember/script/components.h>
#include <ember/script/host.h>
#include <ember/script/lua.h>
#include <ember/script/schema.h>

/**
 * What the module's translation units share and nothing outside it sees: the host's private state,
 * reached through a friend, and the installers each file contributes to the VM.
 */
namespace ember::script
{
	/** Components a query names at most, and filters leave out at most. */
	inline constexpr u32 MAX_QUERY = 8;

	/** Transitions one entity's stategraph may make in one tick before it is stopped and named. */
	inline constexpr u32 MAX_TRANSITIONS = 8;

	/** The engine's own entity verbs, by the atom of their name; a game's entity methods follow them. */
	enum class Verb : i16
	{
		None	= -1,
		Has		= 0,
		Add		= 1,
		Remove	= 2,
		Destroy = 3,
		Id		= 4,
		Play	= 5,
		Event	= 6,
		Count	= 7,
	};

	// --- stategraphs ----------------------------------------------------------------------------------

	/** A tick of a state's timeline: a mark every machine knows the tick of, and the handler the server runs at it. */
	struct Mark
	{
		u32 at	 = 0;
		u64 name = 0;
		int on	 = -1; // the on[mark] function, or none
	};

	/** What a state does with an event: a state to go to, or a function that may name one. */
	struct Rule
	{
		u64 event = 0;
		i16 state = -1;
		int react = -1;
	};

	struct State
	{
		String name{&memory::heap(MemoryTag::Scripting)};
		u64 hash	   = 0;
		int enter	   = -1;
		int update	   = -1;
		int exit	   = -1;
		int next_fn	   = -1;
		i16 next_state = -1;
		u32 every	   = 0; // update every this many ticks; 0 is every tick
		u32 length	   = 0; // ticks before `next`; none while has_length is false
		bool has_length = false;
		Vector<Mark> marks{&memory::heap(MemoryTag::Scripting)}; // by tick
		Vector<Rule> rules{&memory::heap(MemoryTag::Scripting)};

		[[nodiscard]] const Rule* rule(u64 event) const noexcept
		{
			for (const Rule& candidate : rules)
				if (candidate.event == event)
					return &candidate;
			return nullptr;
		}
	};

	/** A stategraph as a module declared it: its states, sorted by name, and where and when it runs. */
	struct Graph
	{
		String name{&memory::heap(MemoryTag::Scripting)};
		GraphId id;
		u32 module		= 0;
		Context context = Context::Server;
		u8 stage		= 0;
		i16 initial		= -1;
		bool disabled	= false;
		Vector<State> states{&memory::heap(MemoryTag::Scripting)};

		[[nodiscard]] i16 find(StringView state) const noexcept
		{
			for (size_t i = 0; i < states.size(); ++i)
				if (states[i].name == state)
					return static_cast<i16>(i);
			return -1;
		}
	};

	/** An event raised on an entity, waiting for the next script point. */
	struct Event
	{
		ecs::Entity target = ecs::NO_ENTITY;
		ecs::Entity source = ecs::NO_ENTITY;
		u64 name		   = 0;
		u32 tick		   = 0; // raised at
		bool applied	   = false; // its prefab event, if any, has been
	};

	/** A prefab's groups and events, as the running host converted them: what e:event() swaps. */
	struct Extras
	{
		ecs::PrefabId prefab = 0;
		u32 module			 = 0;
		Vector<ecs::PrefabComponent> own{&memory::heap(MemoryTag::Scripting)}; // without any group: what a removed group's component goes back to
		struct Group
		{
			u64 name = 0;
			Vector<ecs::PrefabComponent> components{&memory::heap(MemoryTag::Scripting)};
		};
		struct Swap
		{
			u64 name = 0;
			Vector<u32> add{&memory::heap(MemoryTag::Scripting)};	 // group indices
			Vector<u32> remove{&memory::heap(MemoryTag::Scripting)};
		};
		Vector<Group> groups{&memory::heap(MemoryTag::Scripting)};
		Vector<Swap> events{&memory::heap(MemoryTag::Scripting)};
	};

	/** A prefab declared in schema mode, converted once every component is known: finish_schema(). */
	struct PendingPrefab
	{
		int table_ref = -1;
		String name{&memory::heap(MemoryTag::Scripting)};
		String path{&memory::heap(MemoryTag::Scripting)};
		u32 line = 0;
	};

	struct Host::Brains
	{
		Brains() noexcept {}

		Vector<Graph> graphs{&memory::heap(MemoryTag::Scripting)};
		Vector<Graph> staging{&memory::heap(MemoryTag::Scripting)}; // the loading module's, until it loads
		Vector<Extras> extras{&memory::heap(MemoryTag::Scripting)};
		Vector<Extras> extras_staging{&memory::heap(MemoryTag::Scripting)}; // the loading module's, until it loads
		Vector<Event> events{&memory::heap(MemoryTag::Scripting)};
		Vector<Event> raised{&memory::heap(MemoryTag::Scripting)}; // during a run: for the next point
		HashMap<u32, u32> draws{&memory::heap(MemoryTag::Scripting)}; // an entity's dice rolls this tick
		u32 draws_tick = 0;
		bool running   = false; // inside run_events(): what e:event() raises waits
		ecs::Entity stepping = ecs::NO_ENTITY; // the entity whose graph is being stepped
		bool went			 = false;		   // Stategraph:go() moved it mid-step

		const ecs::ComponentInfo* stategraph = nullptr; // the engine components, when the game registered them
		const ecs::ComponentInfo* playing	 = nullptr;
		i16 atom_rng						 = -1;

		// Schema mode: what the declarations left for finish_schema().
		Vector<PendingPrefab> pending{&memory::heap(MemoryTag::Scripting)};
		Vector<ecs::ComponentInfo> described{&memory::heap(MemoryTag::Scripting)}; // the collected components' layouts
		Vector<Exposed> described_exposed{&memory::heap(MemoryTag::Scripting)};

		[[nodiscard]] Graph* graph(GraphId id) noexcept
		{
			for (Graph& candidate : graphs)
				if (candidate.id == id)
					return &candidate;
			return nullptr;
		}

		[[nodiscard]] Extras* extras_of(ecs::PrefabId prefab) noexcept
		{
			for (Extras& candidate : extras)
				if (candidate.prefab == prefab)
					return &candidate;
			return nullptr;
		}
	};

	struct HostAccess
	{
		[[nodiscard]] static Host& of(lua_State* L) noexcept { return *static_cast<Host*>(lua_callbacks(L)->userdata); }
		[[nodiscard]] static Context context(const Host& host) noexcept { return host.m_context; }
		[[nodiscard]] static ecs::Commands* commands(Host& host) noexcept { return host.m_commands; }
		[[nodiscard]] static bool loading(const Host& host) noexcept { return host.m_loading >= 0; }
		[[nodiscard]] static i32 loading_module(const Host& host) noexcept { return host.m_loading; }
		[[nodiscard]] static Context loading_context(const Host& host) noexcept
		{
			return host.m_loading >= 0 ? host.m_modules[static_cast<size_t>(host.m_loading)].context : Context::Count;
		}
		[[nodiscard]] static StringView loading_path(const Host& host) noexcept
		{
			return host.m_loading >= 0 ? StringView(host.m_modules[static_cast<size_t>(host.m_loading)].path) : StringView();
		}
		[[nodiscard]] static Stats& stats(Host& host) noexcept { return host.m_stats; }
		[[nodiscard]] static const HostDef& def(const Host& host) noexcept { return host.m_def; }
		[[nodiscard]] static Binding& binding(Host& host) noexcept { return host.m_binding; }
		[[nodiscard]] static const Binding& binding(const Host& host) noexcept { return host.m_binding; }
		[[nodiscard]] static Host::Brains& brains(Host& host) noexcept { return *host.m_brains; }
		[[nodiscard]] static u32 now(const Host& host) noexcept { return host.m_now; }
		[[nodiscard]] static i16 atom(Host& host, StringView name) noexcept { return host.atom(name); }
		static void report(Host& host, StringView path, u32 line, Severity severity, StringView message) noexcept
		{
			host.report(path, line, severity, message);
		}
		static void report_lua(Host& host, StringView path, const char* message) noexcept { host.report_lua(path, message); }

		/** Around a call into a module's function from the host's own loops: its rights, and a fresh budget. */
		static void begin_call(Host& host, Context context) noexcept
		{
			host.m_context = context;
			host.m_steps   = 0;
		}
		static void end_call(Host& host) noexcept { host.m_context = Context::Count; }
		[[nodiscard]] static lua_State* thread_of(const Host& host, u32 module) noexcept
		{
			return host.m_modules[module].thread;
		}
		[[nodiscard]] static StringView path_of(const Host& host, u32 module) noexcept { return host.m_modules[module].path; }

		/** The component an atom names, or NO_COMPONENT. */
		[[nodiscard]] static ecs::ComponentId component_of_atom(const Host& host, int atom) noexcept
		{
			return atom >= 0 && static_cast<size_t>(atom) < host.m_component_by_atom.size()
					   ? host.m_component_by_atom[static_cast<size_t>(atom)]
					   : ecs::NO_COMPONENT;
		}

		/** An entity verb by atom: an engine Verb, or Verb::Count + the index of a game entity method. */
		[[nodiscard]] static i16 verb_of_atom(const Host& host, int atom) noexcept
		{
			return atom >= 0 && static_cast<size_t>(atom) < host.m_verb_by_atom.size()
					   ? host.m_verb_by_atom[static_cast<size_t>(atom)]
					   : static_cast<i16>(Verb::None);
		}
	};

	// --- rights -------------------------------------------------------------------------------------

	/** Whether a script of this context may write (or add, or remove) this component. */
	[[nodiscard]] bool may_write(Context context, const Exposed& exposed) noexcept;

	/** Raises the error that says why a write of this component was refused. Never returns. */
	[[noreturn]] void refuse_write(lua_State* L, Context context, const Exposed& exposed, StringView what);

	/** Raises when the function may not be called from the running context. */
	void check_context(lua_State* L, const Host& host, ContextMask where, const char* name);

	/** A call that needs the world open: inside a system. Raises outside one. */
	[[nodiscard]] ecs::Commands& check_commands(lua_State* L, Host& host, const char* what);

	/** Whether a module of this context runs in a world of this role: server scripts never on a client, and so on. */
	[[nodiscard]] bool runs_in(Context context, ecs::Role role) noexcept;

	// --- fields (binding.cpp) -------------------------------------------------------------------------

	/** The field's value, from the component's bytes, onto the stack. */
	void push_field(lua_State* L, const Field& field, const void* bytes);

	/** The value at `index` into the field of the component's bytes; raises on a type or range mismatch. */
	void write_field(lua_State* L, const Exposed& exposed, const Field& field, void* bytes, int index, bool checks);

	/**
	 * A table of fields at `table` onto the component's bytes, as e:add(T, {...}) and world:spawn() take. Given
	 * `written`, a byte per byte of the component, the bytes of every field given are marked 1.
	 */
	void fill_component(lua_State* L, const Exposed& exposed, int table, void* bytes, bool checks, u8* written = nullptr);

	/** An Exposed's fields from a dynamic type's layout, with the host's atoms. */
	void describe_fields(Host& host, const ecs::ComponentInfo& info, Exposed& exposed) noexcept;

	/** The entity a handle names, which must still exist. Raises when it is gone. */
	[[nodiscard]] ecs::Entity live_entity(lua_State* L, ecs::World& world, int index);

	// --- prefabs (schema.cpp) -------------------------------------------------------------------------

	/**
	 * A declared prefab's components: the base's, with the declaration's entries over them field by field,
	 * then its start groups' when asked; sorted by id. False, with why, when an entry names no type.
	 */
	[[nodiscard]] bool compose_prefab(const ecs::Components& types, const ecs::Prefab* base, const PrefabDecl& decl,
									  bool with_start_groups, Vector<ecs::PrefabComponent>& out, String& why) noexcept;

	// --- declarations (declare.cpp) -------------------------------------------------------------------

	/** component, prefab and stategraph, and the unit declarators tiles(), ticks(), seconds(), count(), tick(), int(). */
	void install_declarations(lua_State* L, Host& host);

	/** A declaration's number: a plain number, or a unit value {__unit, n} from the schema pass. Raises otherwise. */
	[[nodiscard]] f64 declared_number(lua_State* L, int index, const char* what);

	// --- stategraphs (stategraph.cpp) -----------------------------------------------------------------

	/** Reads a `stategraph "x" { ... }` table at `index` into `out`, with its functions referenced. Raises on a mistake. */
	void read_graph(lua_State* L, Host& host, int index, Graph& out);

	/** Lets a graph's references go. */
	void release_graph(lua_State* L, Graph& graph) noexcept;

	/** Runs every stategraph of this stage over the entities that carry it, then delivers what is left. */
	void run_stategraphs(Host& host, u8 stage) noexcept;

	/** The Stategraph component's methods on the binding: name(), elapsed(), tick_of() and the rest. */
	void expose_stategraph(Binding& binding) noexcept;

	/** The rng handle's name, and the engine verbs' helpers. */
	void install_stategraphs(lua_State* L, Host& host);
	int rng_namecall(lua_State* L);
	int entity_play(lua_State* L, Host& host, ecs::Entity entity);
	int entity_event(lua_State* L, Host& host, ecs::Entity entity);

	// --- installers ---------------------------------------------------------------------------------

	/** The one metatable every handle shares, and the names typeof() gives them. */
	void install_handles(lua_State* L, Host& host);

	/** The `world` table: query and spawn; and `without`. */
	void install_world(lua_State* L, Host& host);

	/** The Shape userdata, the `shape` library and `layers()`. */
	void install_physics(lua_State* L, Host& host);

	void push_shape(lua_State* L, const physics::Shape& shape);
	[[nodiscard]] const physics::Shape& check_shape(lua_State* L, int index);

	/** A shape's members into `bytes`, over zeroes: none of the source's padding, so every machine's bytes agree. */
	void store_shape(void* bytes, const physics::Shape& shape) noexcept;

	/** The ember.d.luau text for this binding (definitions.cpp). */
	void write_definitions_text(const Binding& binding, String& out);
}
