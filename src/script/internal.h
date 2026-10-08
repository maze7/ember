#pragma once

#include <ember/anim/components.h>
#include <ember/net/replication.h>
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
		None	 = -1,
		Has		 = 0,
		Add		 = 1,
		Remove	 = 2,
		Destroy	 = 3,
		Id		 = 4,
		Play	 = 5,
		Event	 = 6,
		Start	 = 14, // stories
		Stop	 = 15,
		Exists	 = 16,
		Strike	 = 17, // stategraphs
		Inflict	 = 18, // modifiers
		Cure	 = 19,
		Modifier = 20,
		Stat	 = 21,
		Prefab	 = 22, // what it was made from
		// A client's, in show handlers: what the entity looks and sounds like.
		Flash	= 7,
		Overlay = 8,
		Squash	= 9,
		Lean	= 10,
		Hold	= 11,
		Sound	= 12,
		Mine	= 13,
		Count	= 23,
	};

	/** A show's answer to a mark or a cue by name. */
	struct ShowRule
	{
		u64 name = 0;
		int fn	 = -1;
	};

	/**
	 * What clients show of a state, a graph or a prefab: handlers at its entry and exit and every frame,
	 * and at its marks and the cues raised on it, by name. Loaded on every machine; run on clients, at the
	 * moment each is drawn.
	 */
	struct Show
	{
		int enter  = -1;
		int exit   = -1;
		int update = -1;
		Vector<ShowRule> on{&memory::heap(MemoryTag::Scripting)};

		[[nodiscard]] bool empty() const noexcept { return enter < 0 && exit < 0 && update < 0 && on.empty(); }
		[[nodiscard]] int rule(u64 name) const noexcept
		{
			for (const ShowRule& candidate : on)
				if (candidate.name == name)
					return candidate.fn;
			return -1;
		}

		/** The same by the low half of a name's hash, which is all a replicated cue carries. */
		[[nodiscard]] int rule_low(u32 low) const noexcept
		{
			for (const ShowRule& candidate : on)
				if (static_cast<u32>(candidate.name) == low)
					return candidate.fn;
			return -1;
		}
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
		i16 state = -1; // resolved from state_name once every state is known
		int react = -1;
		String state_name{&memory::heap(MemoryTag::Scripting)}; // `to "x"`, or events = { e = "x" }
		bool with_source = false; // the first form's react: the handler takes the source entity, not the event
		bool maybe_mark =
			false; // an `on` key of a state whose marks a function makes: a mark's handler, if one has the name
	};

	struct State
	{
		String name{&memory::heap(MemoryTag::Scripting)};
		u64 hash	   = 0;
		StateId id	   = NO_STATE; // the low half of hash: what the component holds
		int enter	   = -1;
		int update	   = -1;
		int exit	   = -1;
		int next_fn	   = -1;
		int length_fn  = -1; // length = function(e)
		int marks_fn   = -1; // marks = function(e)
		String next_name{&memory::heap(MemoryTag::Scripting)};
		i16 next_state = -1;
		u32 every	   = 0; // update every this many ticks; 0 is every tick
		u32 length	   = 0; // ticks before `next`; none while has_length is false
		u64 length_mark = 0; // length = "mark": the mark's tick
		bool has_every	= false;
		bool has_length = false;
		bool has_next	= false;
		bool has_marks	= false;
		bool has_ignore = false;
		Vector<Mark> marks{&memory::heap(MemoryTag::Scripting)}; // by tick
		Vector<Rule> rules{&memory::heap(MemoryTag::Scripting)};
		Vector<u64> ignored{&memory::heap(MemoryTag::Scripting)}; // events the graph answers that this state does not
		Show show;

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
		String initial_name{&memory::heap(MemoryTag::Scripting)};
		String extends{&memory::heap(MemoryTag::Scripting)}; // a graph whose states this one starts from
		bool disabled	= false;
		bool show_disabled = false; // a show handler failed: off until the module reloads
		bool finished	   = false; // its states sorted and its names resolved: finish_graph()
		Vector<State> states{&memory::heap(MemoryTag::Scripting)};
		Vector<Rule> rules{
			&memory::heap(MemoryTag::Scripting)}; // in every state that neither answers nor ignores the event
		int update = -1;						  // every tick in every state, after the state's own
		u32 every  = 0;
		Show show; // in every state

		[[nodiscard]] i16 find(StringView state) const noexcept
		{
			for (size_t i = 0; i < states.size(); ++i)
				if (states[i].name == state)
					return static_cast<i16>(i);
			return -1;
		}

		/** The state a component's id names; -1 for none, or one this version of the graph lost. */
		[[nodiscard]] i16 index_of(StateId id) const noexcept
		{
			if (id == NO_STATE)
				return -1;
			for (size_t i = 0; i < states.size(); ++i)
				if (states[i].id == id)
					return static_cast<i16>(i);
			return -1;
		}
	};

	/** An inspector's push into a state: an event no state answers or ignores. */
	inline constexpr u64 FORCE_EVENT = hash_text("@force");

	/**
	 * An event raised on an entity, waiting for the next script point. Each of the entity's graph slots and
	 * stories sees it once, answered or not; it goes two ticks after it was raised.
	 */
	struct Event
	{
		ecs::Entity target = ecs::NO_ENTITY;
		ecs::Entity source = ecs::NO_ENTITY;
		u64 name		   = 0;
		u32 tick		   = 0; // raised at
		u32 serial		   = 0; // in the order raised: what a story has seen up to
		f32 value		   = 0.0f;
		StateId forced	   = NO_STATE; // FORCE_EVENT's state
		u8 slot			   = 0;		   // FORCE_EVENT's slot
		u8 delivered	   = 0;		   // a bit per graph slot that has seen it
		bool applied	   = false;	   // its prefab event, if any, has been
	};

	/** The marks a state's function made for an entity, as of the state and tick it entered. */
	struct EntityMarks
	{
		StateId state = NO_STATE;
		u32 since	  = 0;
		Vector<Mark> marks{&memory::heap(MemoryTag::Scripting)};
	};

	/** A transition, for an inspector: the watched entity's last few. */
	struct Transition
	{
		u8 slot		 = 0;
		StateId from = NO_STATE;
		StateId to	 = NO_STATE;
		u32 tick	 = 0;
	};

	/** A story as a module declared it: its run, its reactions, and what it is attached to. */
	struct StoryDecl
	{
		String name{&memory::heap(MemoryTag::Scripting)};
		u32 id				 = 0; // the low half of its name's hash
		u32 module			 = 0;
		Context context		 = Context::Server;
		u8 stage			 = 0;
		ecs::PrefabId prefab = ecs::NO_PREFAB; // every entity of this prefab runs it
		bool loop			 = false;
		bool disabled		 = false;
		int run				 = -1;
		Vector<ShowRule> on{&memory::heap(MemoryTag::Scripting)};

		[[nodiscard]] int reaction(u64 name) const noexcept
		{
			for (const ShowRule& rule : on)
				if (rule.name == name)
					return rule.fn;
			return -1;
		}
	};

	/** One entity's run of a story: its coroutine, and what it waits for. */
	struct StoryThread
	{
		enum class Wait : u8
		{
			None,
			Start, // made, not yet run: the first resume hands it the entity
			Ticks,
			Until,
			Event
		};

		ecs::Entity entity = ecs::NO_ENTITY;
		u32 story		   = 0;
		lua_State* co	   = nullptr;
		int co_ref		   = -1;
		int until_fn	   = -1;
		Wait wait		   = Wait::None;
		u32 wake		   = 0; // Ticks: the tick it resumes; Until: the next check
		u32 every		   = 1;
		u64 event_name	   = 0;
		u32 line		   = 0; // where it stands, for the inspector
		u32 seen		   = 0; // the serial of the newest event it has seen
		bool failed		   = false;
		bool done		   = false;

		/** What happens to it at the next sweep: a stop or a restart asked for while stories run. */
		enum class Sweep : u8
		{
			None,
			Rest,  // e:stop(): its coroutine goes, and it stays done
			Erase, // a reload or a restart: it goes, and starts over if it is still attached
		};
		Sweep sweep = Sweep::None;
	};

	/** One stat a modifier changes: add first, then multiply, each per stack. */
	struct StatRow
	{
		u64 stat = 0;
		f32 add	 = 0.0f;
		f32 mul	 = 1.0f;
	};

	enum class Stacking : u8
	{
		Refresh, // again: the clock starts over, the stacks stay
		Add,	 // again: one more stack, up to max_stacks, and the clock starts over
		Keep,	 // again: nothing; the first stands
	};

	/** A modifier as a module declared it. */
	struct ModifierDecl
	{
		String name{&memory::heap(MemoryTag::Scripting)};
		u32 id			   = 0; // the low half of its name's hash
		u32 module		   = 0;
		Context context	   = Context::Server;
		u8 stage		   = 0;
		u32 lasts		   = 0; // ticks; 0 until cured
		u16 max_stacks	   = 1;
		u16 power		   = 1; // what e:inflict() gives when it says none
		u32 every		   = 0; // ticks between its tick; 0 for none
		Stacking stacking  = Stacking::Refresh;
		bool disabled	   = false; // its tick failed: off until its module reloads
		bool show_disabled = false; // a show handler failed: the same
		int tick		   = -1;
		Vector<StatRow> stats{&memory::heap(MemoryTag::Scripting)};
		Show show;
	};

	/** A `panel "name" { frame = fn }`: a client script's debug UI, drawn by the game. */
	struct PanelDecl
	{
		String name{&memory::heap(MemoryTag::Scripting)};
		u64 id		  = 0;
		u32 module	  = 0;
		int frame	  = -1;
		int state	  = -1;	   // its state table, which a reload keeps the values of: what the frame is handed
		bool disabled = false; // its frame failed: off until its module reloads
	};

	/** A `show "prefab" { ... }`: what clients show of every entity made from a prefab. */
	struct PrefabShow
	{
		ecs::PrefabId prefab = 0;
		u32 module			 = 0;
		bool disabled		 = false;
		Show show;
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

	/** An entity written into an Entity field before it had a network id: written for real once it has one. */
	struct PendingRef
	{
		ecs::Entity holder		   = ecs::NO_ENTITY;
		ecs::ComponentId component = 0;
		u32 offset				   = 0;
		ecs::Entity target		   = ecs::NO_ENTITY;
	};

	struct Host::Brains
	{
		Brains() noexcept {}

		// Entities by network id, for Entity fields: built from the NetId storage the first time a script point
		// reads one, and again at the next point. Refs to entities with no id yet wait in pending.
		HashMap<u32, ecs::Entity> by_netid{&memory::heap(MemoryTag::Scripting)};
		bool netids_built = false;
		Vector<PendingRef> pending_refs{&memory::heap(MemoryTag::Scripting)};

		/** Every name a script has spelt out, by its hash: what a Name handle prints as. */
		HashMap<u64, String> names{&memory::heap(MemoryTag::Scripting)};

		/** Prefabs retuned by the latest reload(s): the game hands them to its replicator. Cleared by reload(). */
		Vector<ecs::PrefabId> retuned{&memory::heap(MemoryTag::Scripting)};

		Vector<Graph> graphs{&memory::heap(MemoryTag::Scripting)};
		Vector<Graph> staging{&memory::heap(MemoryTag::Scripting)}; // the loading module's, until it loads
		Vector<Extras> extras{&memory::heap(MemoryTag::Scripting)};
		Vector<Extras> extras_staging{&memory::heap(MemoryTag::Scripting)}; // the loading module's, until it loads
		Vector<Event> events{&memory::heap(MemoryTag::Scripting)};
		Vector<Event> raised{&memory::heap(MemoryTag::Scripting)}; // during a run: for the next point
		HashMap<u32, u32> draws{&memory::heap(MemoryTag::Scripting)}; // an entity's dice rolls this tick
		u32 draws_tick	 = 0;
		u32 event_serial = 0;
		bool running	 = false; // inside run_events(): what e:event() raises waits

		// The marks state functions made, by (entity, slot); swept of gone entities now and then.
		HashMap<u64, EntityMarks> entity_marks{&memory::heap(MemoryTag::Scripting)};
		u32 marks_swept = 0;

		// An inspector's: the entity watched, and its last transitions, a ring.
		static constexpr u32 HISTORY = 12;
		ecs::Entity watching		 = ecs::NO_ENTITY;
		Transition history[HISTORY];
		u32 history_next	 = 0;
		u32 history_count	 = 0;
		ecs::Entity stepping = ecs::NO_ENTITY; // the entity whose graph is being stepped
		u32 stepping_slot	 = 0;			   // which of its slots
		bool went			 = false;		   // Stategraph:go() moved it mid-step

		const ecs::ComponentInfo* stategraph = nullptr; // the engine components, when the game registered them
		const ecs::ComponentInfo* playing	 = nullptr;
		const ecs::ComponentInfo* cues		 = nullptr;
		const ecs::ComponentInfo* shown		 = nullptr;
		i16 atom_rng						 = -1;
		i16 atom_state						 = -1; // e.state: its first graph's Stategraph

		// Modifiers: declared per module; the component's info, when the game registered it.
		Vector<ModifierDecl> modifiers{&memory::heap(MemoryTag::Scripting)};
		Vector<ModifierDecl> modifiers_staging{&memory::heap(MemoryTag::Scripting)};
		const ecs::ComponentInfo* modifiers_info = nullptr;
		u8 first_stage							 = 0; // where modifiers are counted down, once a tick

		[[nodiscard]] const ModifierDecl* modifier(u32 id) const noexcept
		{
			for (const ModifierDecl& candidate : modifiers)
				if (candidate.id == id)
					return &candidate;
			return nullptr;
		}
		[[nodiscard]] ModifierDecl* modifier(u32 id) noexcept
		{
			for (ModifierDecl& candidate : modifiers)
				if (candidate.id == id)
					return &candidate;
			return nullptr;
		}

		// Triggers: what each one touched, by (trigger, entity), as of the epoch it last did.
		HashMap<u64, u32> inside{&memory::heap(MemoryTag::Scripting)};
		u32 sense_epoch = 1;

		// Stories: declared per module, and a thread per (entity, story) while it runs.
		Vector<StoryDecl> stories{&memory::heap(MemoryTag::Scripting)};
		Vector<StoryDecl> stories_staging{&memory::heap(MemoryTag::Scripting)};
		HashMap<u64, StoryThread> threads{&memory::heap(MemoryTag::Scripting)}; // by (entity, story)
		StoryThread* resuming = nullptr; // the thread being resumed: whose wait() is being called
		u32 resuming_now	  = 0;

		// Panels: declared per module, sorted by name; and whether one's frame is running, for the ui library.
		Vector<PanelDecl> panels{&memory::heap(MemoryTag::Scripting)};
		Vector<PanelDecl> panels_staging{&memory::heap(MemoryTag::Scripting)};
		bool in_panel = false;

		// Shows: per prefab, and the names any show answers, which the server forwards as cues.
		Vector<PrefabShow> prefab_shows{&memory::heap(MemoryTag::Scripting)};
		Vector<PrefabShow> prefab_shows_staging{&memory::heap(MemoryTag::Scripting)};
		Vector<u64> shown_names{&memory::heap(MemoryTag::Scripting)};

		// The moments a client draws, in ticks (fractions included), set by the game before each Present;
		// and what a predicting client raised on its own entities, shown once, at the next Present.
		f64 predicted	 = 0.0;
		f64 interpolated = 0.0;
		bool presenting	 = false; // set_present() has been called: shows may run
		Vector<Event> own_cues{&memory::heap(MemoryTag::Scripting)};
		f64 moment = 0.0; // the moment the running show handler is for, in ticks

		[[nodiscard]] bool answers(u64 name) const noexcept
		{
			for (const u64 known : shown_names)
				if (known == name)
					return true;
			return false;
		}

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
		static void set_context(Host& host, Context context) noexcept { host.m_context = context; }
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

	/** The field's value, from the component's bytes, onto the stack. An Entity field pushes its id as a number here.
	 */
	void push_field(lua_State* L, const Field& field, const void* bytes);

	/**
	 * The value at `index` into the field of the component's bytes; raises on a type or range mismatch. An
	 * Entity field written with an entity that has no network id yet keeps the ref pending for `holder`'s
	 * component, when a holder is given; raises when none is.
	 */
	void write_field(lua_State* L, const Exposed& exposed, const Field& field, void* bytes, int index, bool checks,
					 ecs::Entity holder = ecs::NO_ENTITY);

	/** A field onto the stack; an Entity field through the host, and nil for none or one that is gone. */
	void push_field(lua_State* L, Host& host, const Field& field, const void* bytes, ecs::Entity holder,
					ecs::ComponentId component);

	/**
	 * A table of fields at `table` onto the component's bytes, as e:add(T, {...}) and world:spawn() take. Given
	 * `written`, a byte per byte of the component, the bytes of every field given are marked 1.
	 */
	void fill_component(lua_State* L, const Exposed& exposed, int table, void* bytes, bool checks, u8* written = nullptr);

	/** An Exposed's fields from a dynamic type's layout, with the host's atoms. */
	void describe_fields(Host& host, const ecs::ComponentInfo& info, Exposed& exposed) noexcept;

	/** The entity a handle names, which must still exist. Raises when it is gone. */
	[[nodiscard]] ecs::Entity live_entity(lua_State* L, ecs::World& world, int index);

	/**
	 * A sim script on a client writes only what that client predicts: raises for an entity that is not
	 * Simulated there, since the server's word would overwrite the write and nothing replays it.
	 */
	void check_sim_target(lua_State* L, Host& host, ecs::Entity entity);

	/** The entity a network id names, through the host's map, which is built on first use each point. */
	[[nodiscard]] ecs::Entity entity_of_netid(Host& host, u32 id) noexcept;

	/** A network id for an entity, or 0 when it has none yet. */
	[[nodiscard]] u32 netid_of(Host& host, ecs::Entity entity) noexcept;

	/** Pending refs whose targets have their ids now are written; those whose ends died are dropped. */
	void settle_pending_refs(Host& host) noexcept;

	/** Hashes and remembers a name, so its handle prints as its text. */
	[[nodiscard]] u64 intern_name(Host& host, StringView text) noexcept;

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

	/**
	 * Once its module's top level has run, and every `g.state` with it: what the graph extends copied in, its
	 * states sorted, its names resolved. False, with why, for a mistake, which refuses the module; warnings for
	 * what is likely one.
	 */
	[[nodiscard]] bool finish_graph(lua_State* L, Host& host, Graph& out, Span<Graph> siblings, String& why,
									Vector<String>& warnings);

	/** An event as a handler gets it: { name, tick, by, n }. */
	void push_event_table(lua_State* L, Host& host, const Event& event);

	/** A transition into the watched entity's history, when it is the one watched. */
	void record_transition(Host& host, ecs::Entity entity, u32 slot, StateId from, StateId to, u32 tick) noexcept;

	/** An inspector's push: the entity's slot into the named state at its graph's next step. */
	[[nodiscard]] bool force_state(Host& host, ecs::Entity entity, u32 slot, StringView name) noexcept;

	/** e:strike{...}. */
	int entity_strike(lua_State* L, Host& host, ecs::Entity entity);

	// --- modifiers (modifier.cpp) -------------------------------------------------------------------------

	/** The entity's modifiers into an inspection. */
	void inspect_modifiers(Host& host, ecs::Entity entity, Inspection& out) noexcept;

	/** Counts every simulated entity's modifiers down, at the first stage; and runs the ticks of this stage's. */
	void run_modifiers(Host& host, u8 stage) noexcept;
	void release_modifier(lua_State* L, ModifierDecl& decl) noexcept;
	int entity_inflict(lua_State* L, Host& host, ecs::Entity entity);
	int entity_cure(lua_State* L, Host& host, ecs::Entity entity);
	int entity_modifier(lua_State* L, Host& host, ecs::Entity entity);
	int entity_stat(lua_State* L, Host& host, ecs::Entity entity);
	[[nodiscard]] f32 stat_of(const Host& host, ecs::Entity entity, u64 stat, f32 base) noexcept;
	void install_modifiers(lua_State* L, Host& host);

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

	/** Reads a `show = { ... }` table at `index` into `out`: enter, exit, update, and the rest by name. Raises on a
	 * mistake. */
	void read_show(lua_State* L, int index, const char* what, Show& out);
	void release_show(lua_State* L, Show& show) noexcept;

	// --- shows (present.cpp) --------------------------------------------------------------------------

	/** Every entity's shows for the frame: states as drawn, their marks, the cues; before the Present systems. */
	void run_shows(Host& host) noexcept;

	/** Gathers the names every loaded show answers, from the graphs and the prefab shows. */
	void gather_shown_names(Host& host) noexcept;

	/** A client's verbs on an entity: flash, overlay, squash, lean, hold, sound, mine. */
	int client_verb(lua_State* L, Host& host, ecs::Entity entity, Verb verb);

	/** e:play(clip) in a client's show or Present system: its Animator plays the clip from the handler's moment. */
	int client_play(lua_State* L, Host& host, ecs::Entity entity, u64 clip);

	/** The `show "prefab" { ... }` declaration. */
	void install_shows(lua_State* L, Host& host);

	// --- panels (panel.cpp) ------------------------------------------------------------------------------

	void release_panel(lua_State* L, PanelDecl& decl) noexcept;

	/** A reloaded panel keeps what its state held: the old table's values over the new declaration's. */
	void keep_panel_state(lua_State* L, const PanelDecl& old, PanelDecl& fresh) noexcept;
	void sort_panels(Host& host) noexcept;
	[[nodiscard]] bool run_panel(Host& host, u32 index) noexcept;
	void install_panels(lua_State* L, Host& host);

	// --- stories (story.cpp) ----------------------------------------------------------------------------

	/** Every story of this stage (or, presenting, every client story) a step on: after the graphs, before the systems.
	 */
	void run_stories(Host& host, u8 stage, bool present) noexcept;
	void release_story(lua_State* L, StoryDecl& decl) noexcept;
	void drop_story_threads(Host& host, u32 story) noexcept;									// starts them over
	void drop_entity_stories(Host& host, ecs::Entity entity, u32 story, bool restart) noexcept; // story 0: all of them
	void sweep_threads(Host& host) noexcept; // what the two above asked for, between resumes
	void story_report(Host& host, ecs::Entity entity, Vector<StoryReport>& out) noexcept;
	int entity_start(lua_State* L, Host& host, ecs::Entity entity);
	int entity_stop(lua_State* L, Host& host, ecs::Entity entity);
	void install_stories(lua_State* L, Host& host);

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
