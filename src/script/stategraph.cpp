#include "internal.h"

#include <ember/core/hash.h>
#include <ember/core/logger.h>

#include <algorithm>
#include <cmath>
#include <cstring>

/**
 * Stategraphs: a module's `stategraph "x" { initial, states }` read into a Graph, and the loop that
 * steps every entity carrying a Stategraph of that graph at the graph's stage. A state has handlers
 * (enter, update, exit), a timeline of marks with handlers at them, a length and what comes next, and
 * the events it answers to. The state and the tick it began live in the component, so nothing is lost
 * to a save, a reload or a late joiner, and every machine with the same scripts steps alike.
 *
 * Also here: e:event(), e:play(), the dice behind e.rng, and the prefab events e:event() triggers.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		constexpr StringView STATE_KEYS[] = {"every", "enter", "update", "exit", "timeline", "on", "length", "next", "events", "react"};
		constexpr StringView GRAPH_KEYS[] = {"initial", "stage", "states"};

		/** A whole number at `index` from `low`; raises naming the key otherwise. */
		[[nodiscard]] u32 read_count(lua_State* L, int index, const char* graph, const char* state, const char* key, f64 low)
		{
			const f64 value = declared_number(L, index, key);
			if (std::floor(value) != value || value < low || value > 4294967295.0)
				luaL_error(L, "stategraph %s, state %s: %s takes a whole number of ticks from %g, not %g", graph, state, key,
						   low, value);
			return static_cast<u32>(value);
		}

		[[nodiscard]] int ref_function(lua_State* L, int index, const char* graph, const char* state, const char* key)
		{
			if (!lua_isfunction(L, index))
				luaL_error(L, "stategraph %s, state %s: %s takes a function, not %s", graph, state, key,
						   luaL_typename(L, index));
			return lua_ref(L, index);
		}

		/** A state's table into a State, with the names it refers to, resolved once every state is known. */
		struct Parsed
		{
			State state;
			String next{&heap()};
			Vector<String> event_names{&heap()};   // the state each `events` entry names
			Vector<u64> event_hashes{&heap()};
			int on_table  = -1; // the `on` table, resolved against the timeline once both are read
			bool has_next = false;
		};

		void read_state(lua_State* L, int table, const char* graph, Parsed& out)
		{
			const char* name = out.state.name.c_str();
			lua_pushnil(L);
			while (lua_next(L, table) != 0)
			{
				if (!lua_isstring(L, -2))
					luaL_error(L, "stategraph %s, state %s: keys are names", graph, name);
				const StringView key = lua_tostring(L, -2);
				const int value		 = lua_gettop(L);

				if (key == "every")
				{
					out.state.every = read_count(L, value, graph, name, "every", 1.0);
				}
				else if (key == "enter")
				{
					out.state.enter = ref_function(L, value, graph, name, "enter");
				}
				else if (key == "update")
				{
					out.state.update = ref_function(L, value, graph, name, "update");
				}
				else if (key == "exit")
				{
					out.state.exit = ref_function(L, value, graph, name, "exit");
				}
				else if (key == "length")
				{
					out.state.length	 = read_count(L, value, graph, name, "length", 0.0);
					out.state.has_length = true;
				}
				else if (key == "next")
				{
					if (lua_isstring(L, value))
						out.next = String(lua_tostring(L, value), &heap());
					else
						out.state.next_fn = ref_function(L, value, graph, name, "next");
					out.has_next = true;
				}
				else if (key == "timeline")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "stategraph %s, state %s: timeline is { [tick] = \"mark\", ... }", graph, name);
					lua_pushnil(L);
					while (lua_next(L, value) != 0)
					{
						if (!lua_isnumber(L, -2) || !lua_isstring(L, -1))
							luaL_error(L, "stategraph %s, state %s: timeline is { [tick] = \"mark\", ... }", graph, name);
						const f64 at = lua_tonumber(L, -2);
						if (std::floor(at) != at || at < 0.0)
							luaL_error(L, "stategraph %s, state %s: a timeline tick is a whole number from 0, not %g", graph,
									   name, at);
						out.state.marks.push_back({.at = static_cast<u32>(at), .name = hash_text(lua_tostring(L, -1))});
						lua_pop(L, 1);
					}
					std::sort(out.state.marks.begin(), out.state.marks.end(),
							  [](const Mark& a, const Mark& b) { return a.at < b.at; });
				}
				else if (key == "on")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "stategraph %s, state %s: on is { mark = function(e) end, ... }", graph, name);
					// Resolved against the timeline below, once both have been read whatever order they came in.
					out.on_table = lua_ref(L, value);
				}
				else if (key == "events")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "stategraph %s, state %s: events is { event = \"state\", ... }", graph, name);
					lua_pushnil(L);
					while (lua_next(L, value) != 0)
					{
						if (!lua_isstring(L, -2) || !lua_isstring(L, -1))
							luaL_error(L, "stategraph %s, state %s: events is { event = \"state\", ... }; a function goes in react",
									   graph, name);
						out.event_hashes.push_back(hash_text(lua_tostring(L, -2)));
						out.event_names.push_back(String(lua_tostring(L, -1), &heap()));
						lua_pop(L, 1);
					}
				}
				else if (key == "react")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "stategraph %s, state %s: react is { event = function(e, source) end, ... }", graph, name);
					lua_pushnil(L);
					while (lua_next(L, value) != 0)
					{
						if (!lua_isstring(L, -2) || !lua_isfunction(L, -1))
							luaL_error(L, "stategraph %s, state %s: react is { event = function(e, source) end, ... }; a state name goes in events",
									   graph, name);
						out.state.rules.push_back({.event = hash_text(lua_tostring(L, -2)), .state = -1, .react = lua_ref(L, -1)});
						lua_pop(L, 1);
					}
				}
				else
				{
					String keys(&heap());
					for (const StringView known : STATE_KEYS)
					{
						if (!keys.empty())
							keys += ", ";
						keys += known;
					}
					luaL_error(L, "stategraph %s, state %s: unknown key '%s'; a state has %s", graph, name, key.data(),
							   keys.c_str());
				}
				lua_pop(L, 1);
			}

			if (out.has_next && !out.state.has_length)
				luaL_error(L, "stategraph %s, state %s: next without length; next is where the state goes when its length is up",
						   graph, name);
			if (out.state.has_length && !out.has_next)
				luaL_error(L, "stategraph %s, state %s: length without next", graph, name);

			// The on handlers onto their marks: one reference per tick the mark stands at.
			if (out.on_table >= 0)
			{
				lua_getref(L, out.on_table);
				const int on = lua_gettop(L);
				lua_pushnil(L);
				while (lua_next(L, on) != 0)
				{
					if (!lua_isstring(L, -2) || !lua_isfunction(L, -1))
						luaL_error(L, "stategraph %s, state %s: on is { mark = function(e) end, ... }", graph, name);
					const u64 mark = hash_text(lua_tostring(L, -2));
					bool found	   = false;
					for (Mark& candidate : out.state.marks)
					{
						if (candidate.name != mark)
							continue;
						candidate.on = lua_ref(L, -1);
						found		 = true;
					}
					if (!found)
						luaL_error(L, "stategraph %s, state %s: on.%s names no mark of the timeline", graph, name,
								   lua_tostring(L, -2));
					lua_pop(L, 1);
				}
				lua_unref(L, out.on_table);
				out.on_table = -1;
				lua_pop(L, 1);
			}
		}
	}

	void read_graph(lua_State* L, Host& host, int index, Graph& out)
	{
		if (index < 0)
			index = lua_gettop(L) + 1 + index;
		const char* graph = out.name.c_str();

		Vector<Parsed> parsed(&heap());
		String initial(&heap());
		bool has_states = false;

		lua_pushnil(L);
		while (lua_next(L, index) != 0)
		{
			if (!lua_isstring(L, -2))
				luaL_error(L, "stategraph %s: keys are names", graph);
			const StringView key = lua_tostring(L, -2);
			const int value		 = lua_gettop(L);

			if (key == "initial")
			{
				if (!lua_isstring(L, value))
					luaL_error(L, "stategraph %s: initial names a state", graph);
				initial = String(lua_tostring(L, value), &heap());
			}
			else if (key == "stage")
			{
				if (!lua_isstring(L, value))
					luaL_error(L, "stategraph %s: stage names a stage of the tick", graph);
				const StageName* stage = HostAccess::binding(host).find_stage(lua_tostring(L, value));
				if (stage == nullptr)
					luaL_error(L, "stategraph %s: no stage named '%s'", graph, lua_tostring(L, value));
				out.stage = stage->index;
			}
			else if (key == "states")
			{
				if (!lua_istable(L, value))
					luaL_error(L, "stategraph %s: states is { name = { ... }, ... }", graph);
				has_states = true;
				lua_pushnil(L);
				while (lua_next(L, value) != 0)
				{
					if (!lua_isstring(L, -2) || !lua_istable(L, -1))
						luaL_error(L, "stategraph %s: a state is a name and a table", graph);
					Parsed& state	 = parsed.emplace_back();
					state.state.name = String(lua_tostring(L, -2), &heap());
					state.state.hash = hash_text(state.state.name);
					read_state(L, lua_gettop(L), graph, state);
					lua_pop(L, 1);
				}
			}
			else
			{
				luaL_error(L, "stategraph %s: unknown key '%s'; a stategraph has initial, stage and states", graph, key.data());
			}
			lua_pop(L, 1);
		}

		if (!has_states || parsed.empty())
			luaL_error(L, "stategraph %s: no states", graph);
		if (parsed.size() > Stategraph::NO_STATE)
			luaL_error(L, "stategraph %s: %d states; the wire carries %d at most", graph, static_cast<int>(parsed.size()),
					   static_cast<int>(Stategraph::NO_STATE));
		if (initial.empty())
			luaL_error(L, "stategraph %s: no initial state", graph);

		// The stage: the one asked for, or the first of the tick.
		bool stage_given = false;
		lua_rawgetfield(L, index, "stage");
		stage_given = !lua_isnil(L, -1);
		lua_pop(L, 1);
		if (!stage_given)
		{
			const Span<const StageName> stages = HostAccess::binding(host).stages();
			if (stages.empty())
				luaL_error(L, "stategraph %s: the game registered no stages", graph);
			u8 first = stages[0].index;
			for (const StageName& stage : stages)
				first = std::min(first, stage.index);
			out.stage = first;
		}

		// States sorted by name: the index every machine agrees on, whatever order the file wrote them in.
		std::sort(parsed.begin(), parsed.end(),
				  [](const Parsed& a, const Parsed& b) { return a.state.name < b.state.name; });
		for (Parsed& state : parsed)
			out.states.push_back(std::move(state.state));

		out.initial = out.find(initial);
		if (out.initial < 0)
			luaL_error(L, "stategraph %s: initial state '%s' is not among the states", graph, initial.c_str());

		for (size_t i = 0; i < parsed.size(); ++i)
		{
			State& state = out.states[i];
			if (parsed[i].has_next && state.next_fn < 0)
			{
				state.next_state = out.find(parsed[i].next);
				if (state.next_state < 0)
					luaL_error(L, "stategraph %s, state %s: next names '%s', which is not a state", graph, state.name.c_str(),
							   parsed[i].next.c_str());
			}
			for (size_t j = 0; j < parsed[i].event_names.size(); ++j)
			{
				const i16 target = out.find(parsed[i].event_names[j]);
				if (target < 0)
					luaL_error(L, "stategraph %s, state %s: an event names state '%s', which is not one", graph,
							   state.name.c_str(), parsed[i].event_names[j].c_str());
				if (state.rule(parsed[i].event_hashes[j]) != nullptr)
					luaL_error(L, "stategraph %s, state %s: an event is in both events and react", graph, state.name.c_str());
				state.rules.push_back({.event = parsed[i].event_hashes[j], .state = target, .react = -1});
			}
		}
	}

	void release_graph(lua_State* L, Graph& graph) noexcept
	{
		for (State& state : graph.states)
		{
			for (const int ref : {state.enter, state.update, state.exit, state.next_fn})
				if (ref >= 0)
					lua_unref(L, ref);
			for (const Mark& mark : state.marks)
				if (mark.on >= 0)
					lua_unref(L, mark.on);
			for (const Rule& rule : state.rules)
				if (rule.react >= 0)
					lua_unref(L, rule.react);
		}
		graph.states.clear();
	}

	// --- the step -----------------------------------------------------------------------------------

	namespace
	{
		/**
		 * Calls one of a graph's functions on its module's thread with the entity (and the event's
		 * source, when there is one). What it returns, if a string, names the next state in `target`.
		 * False on an error, which disables the graph until its module reloads.
		 */
		[[nodiscard]] bool call(Host& host, Graph& graph, int ref, ecs::Entity entity, ecs::Entity source, bool with_source,
								i16& target, const char* what, const State& state)
		{
			lua_State* thread = HostAccess::thread_of(host, graph.module);
			lua_settop(thread, 0);
			lua_getref(thread, ref);
			push_entity(thread, entity);
			if (with_source)
			{
				if (source == ecs::NO_ENTITY || !host.world().registry.valid(source))
					lua_pushnil(thread);
				else
					push_entity(thread, source);
			}

			HostAccess::begin_call(host, graph.context);
			const int status = lua_pcall(thread, with_source ? 2 : 1, 1, 0);
			HostAccess::end_call(host);
			++HostAccess::stats(host).calls;

			const StringView path = HostAccess::path_of(host, graph.module);
			if (status != LUA_OK)
			{
				HostAccess::report_lua(host, path, lua_tostring(thread, -1));
				lua_settop(thread, 0);
				graph.disabled = true;
				++HostAccess::stats(host).errors;
				return false;
			}

			if (!lua_isnoneornil(thread, -1))
			{
				const char* name = lua_type(thread, -1) == LUA_TSTRING ? lua_tostring(thread, -1) : nullptr;
				target			 = name != nullptr ? graph.find(name) : static_cast<i16>(-1);
				if (target < 0)
				{
					String message(&heap());
					message += "stategraph ";
					message += graph.name;
					message += ", state ";
					message += state.name;
					message += ": ";
					message += what;
					message += " returned ";
					if (name != nullptr)
					{
						message += "'";
						message += name;
						message += "', which is not a state";
					}
					else
					{
						message += "a ";
						message += luaL_typename(thread, -1);
						message += "; a handler returns the next state's name, or nothing";
					}
					HostAccess::report(host, path, 0, Severity::Error, message);
					lua_settop(thread, 0);
					graph.disabled = true;
					++HostAccess::stats(host).errors;
					return false;
				}
			}
			lua_settop(thread, 0);
			return true;
		}

		/** Runs the handlers of every mark at `at`; false on an error. */
		[[nodiscard]] bool fire_marks(Host& host, Graph& graph, State& state, ecs::Entity entity, u32 at)
		{
			for (const Mark& mark : state.marks)
			{
				if (mark.at != at || mark.on < 0)
					continue;
				i16 ignored = -1;
				if (!call(host, graph, mark.on, entity, ecs::NO_ENTITY, false, ignored, "a mark handler", state))
					return false;
				if (HostAccess::brains(host).went)
					return true;
			}
			return true;
		}

		/** One tick of one entity's graph: its events, then its state's handlers, through any transitions. */
		void step(Host& host, Graph& graph, ecs::Entity entity, Stategraph& sg, u32 now)
		{
			Host::Brains& brains = HostAccess::brains(host);
			i16 target			 = -1;

			if (sg.state != Stategraph::NO_STATE && sg.state >= graph.states.size())
				sg.state = Stategraph::NO_STATE; // the graph lost states in a reload: start again

			// The events raised on it since its last step, each handled by the state it is in as it is read;
			// one already moving it on drops the rest, as a state that is left hears nothing more.
			if (sg.state != Stategraph::NO_STATE)
			{
				for (size_t i = 0; i < brains.events.size();)
				{
					if (brains.events[i].target != entity)
					{
						++i;
						continue;
					}
					const Event taken = brains.events[i];
					brains.events.erase(brains.events.begin() + static_cast<std::ptrdiff_t>(i));
					if (target >= 0 || brains.went)
						continue;

					State& state	 = graph.states[sg.state];
					const Rule* rule = state.rule(taken.name);
					if (rule == nullptr)
						continue;
					if (rule->state >= 0)
						target = rule->state;
					else if (rule->react >= 0 &&
							 !call(host, graph, rule->react, entity, taken.source, true, target, "a react handler", state))
						return;
				}
			}

			bool fresh = false;
			for (u32 transitions = 0;;)
			{
				if (brains.went)
				{
					// Stategraph:go() moved it from a handler: its exit has run; the new state is entered here.
					brains.went = false;
					target		= -1;
					fresh		= true;
					++transitions;
				}
				if (sg.state == Stategraph::NO_STATE)
					target = graph.initial;

				if (target >= 0)
				{
					if (++transitions > MAX_TRANSITIONS)
					{
						String message(&heap());
						message += "stategraph ";
						message += graph.name;
						message += ": more than ";
						message += std::to_string(MAX_TRANSITIONS).c_str();
						message += " transitions in one tick: states that pass straight on to each other in a loop";
						HostAccess::report(host, HostAccess::path_of(host, graph.module), 0, Severity::Error, message);
						graph.disabled = true;
						++HostAccess::stats(host).errors;
						return;
					}
					if (sg.state != Stategraph::NO_STATE && graph.states[sg.state].exit >= 0)
					{
						i16 ignored = -1;
						if (!call(host, graph, graph.states[sg.state].exit, entity, ecs::NO_ENTITY, false, ignored, "exit",
								  graph.states[sg.state]))
							return;
					}
					sg.state = static_cast<u8>(target);
					sg.since = now;
					target	 = -1;
					fresh	 = true;
					++HostAccess::stats(host).transitions;
				}

				State& state	  = graph.states[sg.state];
				const u32 elapsed = now >= sg.since ? now - sg.since : 0;

				if (fresh)
				{
					fresh = false;
					if (state.enter >= 0)
					{
						if (!call(host, graph, state.enter, entity, ecs::NO_ENTITY, false, target, "enter", state))
							return;
						if (brains.went || target >= 0)
							continue;
					}
					if (!fire_marks(host, graph, state, entity, 0))
						return;
					if (brains.went)
						continue;
				}
				else
				{
					if (state.has_length && elapsed >= state.length)
					{
						if (state.next_state >= 0)
							target = state.next_state;
						else if (state.next_fn >= 0 &&
								 !call(host, graph, state.next_fn, entity, ecs::NO_ENTITY, false, target, "next", state))
							return;
						if (brains.went || target >= 0)
							continue;

						String message(&heap());
						message += "stategraph ";
						message += graph.name;
						message += ", state ";
						message += state.name;
						message += ": next returned no state when the length was up";
						HostAccess::report(host, HostAccess::path_of(host, graph.module), 0, Severity::Error, message);
						graph.disabled = true;
						return;
					}
					if (!fire_marks(host, graph, state, entity, elapsed))
						return;
					if (brains.went)
						continue;
				}

				if (state.update >= 0 && (state.every == 0 || elapsed % state.every == 0))
				{
					if (!call(host, graph, state.update, entity, ecs::NO_ENTITY, false, target, "update", state))
						return;
					if (brains.went || target >= 0)
						continue;
				}
				break;
			}
		}

		/** A prefab event's groups onto an entity, through the commands: the removed groups' components back to the prefab's or gone, the added groups' in. */
		void apply_prefab_event(Host& host, const Event& event)
		{
			ecs::World& world	 = host.world();
			Host::Brains& brains = HostAccess::brains(host);
			ecs::Commands* commands = HostAccess::commands(host);
			if (commands == nullptr || !world.registry.valid(event.target))
				return;

			const ecs::PrefabRef* ref = world.registry.try_get<ecs::PrefabRef>(event.target);
			if (ref == nullptr)
				return;
			Extras* extras = brains.extras_of(static_cast<ecs::PrefabId>(ref->id));
			if (extras == nullptr)
				return;

			const Extras::Swap* swap = nullptr;
			for (const Extras::Swap& candidate : extras->events)
				if (candidate.name == event.name)
					swap = &candidate;
			if (swap == nullptr)
				return;

			for (const u32 group : swap->remove)
			{
				for (const ecs::PrefabComponent& component : extras->groups[group].components)
				{
					bool added = false;
					for (const u32 other : swap->add)
						for (const ecs::PrefabComponent& candidate : extras->groups[other].components)
							added = added || candidate.id == component.id;
					if (added)
						continue;

					// Back to what the prefab has without the group, or gone when it has nothing.
					const ecs::PrefabComponent* own = nullptr;
					for (const ecs::PrefabComponent& candidate : extras->own)
						if (candidate.id == component.id)
							own = &candidate;
					if (own != nullptr)
						commands->add(event.target, component.id, own->value.data());
					else
						commands->remove(event.target, component.id);
				}
			}
			for (const u32 group : swap->add)
				for (const ecs::PrefabComponent& component : extras->groups[group].components)
					commands->add(event.target, component.id, component.value.data());
			++HostAccess::stats(host).writes;
		}
	}

	void run_stategraphs(Host& host, u8 stage) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		ecs::World& world	 = host.world();
		const u32 now		 = HostAccess::now(host);

		// Prefab events first: what e:event("frenzy") swaps is in place before any graph looks.
		for (Event& event : brains.events)
		{
			if (event.applied)
				continue;
			event.applied = true;
			apply_prefab_event(host, event);
		}

		if (brains.stategraph != nullptr && !brains.graphs.empty())
		{
			const entt::sparse_set* storage = world.registry.storage(brains.stategraph->type);
			if (storage != nullptr)
			{
				brains.running = true;
				for (u32 i = static_cast<u32>(storage->size()); i-- > 0;)
				{
					const ecs::Entity entity = (*storage)[i];
					auto* sg = static_cast<Stategraph*>(brains.stategraph->get(*brains.stategraph, world.registry, entity));
					if (sg == nullptr)
						continue;

					Graph* graph = brains.graph(sg->graph);
					if (graph == nullptr || graph->disabled || graph->stage != stage || !runs_in(graph->context, world.role()))
						continue;
					if (!world.registry.all_of<ecs::Simulated>(entity))
						continue;

					brains.stepping = entity;
					brains.went		= false;
					step(host, *graph, entity, *sg, now);
				}
				brains.stepping = ecs::NO_ENTITY;
				brains.running	= false;
			}
		}

		// What handlers raised waits for the next point; what nobody took in two ticks is dropped.
		for (Event& raised : brains.raised)
			brains.events.push_back(raised);
		brains.raised.clear();
		std::erase_if(brains.events, [&](const Event& event) { return now - event.tick >= 2; });
	}

	// --- the Stategraph component's methods -----------------------------------------------------------

	namespace
	{
		[[nodiscard]] Graph* graph_of(lua_State* L, const Stategraph& sg) noexcept
		{
			return HostAccess::brains(HostAccess::of(L)).graph(sg.graph);
		}

		int sg_name(lua_State* L, Stategraph& sg, ecs::Entity)
		{
			const Graph* graph = graph_of(L, sg);
			if (graph == nullptr || sg.state == Stategraph::NO_STATE || sg.state >= graph->states.size())
				lua_pushstring(L, "");
			else
				lua_pushlstring(L, graph->states[sg.state].name.data(), graph->states[sg.state].name.size());
			return 1;
		}

		int sg_graph(lua_State* L, Stategraph& sg, ecs::Entity)
		{
			const Graph* graph = graph_of(L, sg);
			if (graph == nullptr)
				lua_pushstring(L, "");
			else
				lua_pushlstring(L, graph->name.data(), graph->name.size());
			return 1;
		}

		int sg_elapsed(lua_State* L, Stategraph& sg, ecs::Entity)
		{
			const u32 now = HostAccess::now(HostAccess::of(L));
			lua_pushnumber(L, sg.state == Stategraph::NO_STATE || now < sg.since ? 0.0 : static_cast<f64>(now - sg.since));
			return 1;
		}

		[[nodiscard]] const State& current_state(lua_State* L, const Stategraph& sg, const Graph*& graph)
		{
			graph = graph_of(L, sg);
			if (graph == nullptr)
				luaL_error(L, "Stategraph: no loaded stategraph drives this entity");
			if (sg.state == Stategraph::NO_STATE || sg.state >= graph->states.size())
				luaL_error(L, "Stategraph: not in a state yet");
			return graph->states[sg.state];
		}

		int sg_tick_of(lua_State* L, Stategraph& sg, ecs::Entity)
		{
			const char* mark = luaL_checkstring(L, 2);
			const Graph* graph;
			const State& state = current_state(L, sg, graph);
			const u64 name	   = hash_text(mark);
			for (const Mark& candidate : state.marks)
			{
				if (candidate.name == name)
				{
					lua_pushnumber(L, static_cast<f64>(sg.since) + static_cast<f64>(candidate.at));
					return 1;
				}
			}
			luaL_error(L, "Stategraph:tick_of: state %s has no mark '%s'", state.name.c_str(), mark);
		}

		int sg_ticks_of(lua_State* L, Stategraph& sg, ecs::Entity)
		{
			const char* mark = luaL_checkstring(L, 2);
			const Graph* graph;
			const State& state = current_state(L, sg, graph);
			const u64 name	   = hash_text(mark);
			lua_newtable(L);
			int count = 0;
			for (const Mark& candidate : state.marks)
			{
				if (candidate.name != name)
					continue;
				lua_pushnumber(L, static_cast<f64>(sg.since) + static_cast<f64>(candidate.at));
				lua_rawseti(L, -2, ++count);
			}
			return 1;
		}

		int sg_go(lua_State* L, Stategraph& sg, ecs::Entity self)
		{
			Host& host			 = HostAccess::of(L);
			Host::Brains& brains = HostAccess::brains(host);
			const char* name	 = luaL_checkstring(L, 2);
			if (!brains.running || brains.stepping != self)
				luaL_error(L, "Stategraph:go is for the graph's own handlers; from anywhere else, raise an event: e:event(\"%s\")",
						   name);

			const Graph* graph;
			const State& state = current_state(L, sg, graph);
			const i16 target   = graph->find(name);
			if (target < 0)
				luaL_error(L, "Stategraph:go: '%s' is not a state of %s", name, graph->name.c_str());

			// Its exit runs here, on the caller's stack; the step enters the new state when the handler returns.
			if (state.exit >= 0)
			{
				lua_getref(L, state.exit);
				push_entity(L, self);
				lua_call(L, 1, 0);
			}
			sg.state	= static_cast<u8>(target);
			sg.since	= HostAccess::now(host);
			brains.went = true;
			++HostAccess::stats(host).transitions;
			return 0;
		}
	}

	void expose_stategraph(Binding& binding) noexcept
	{
		binding.expose<Stategraph>();
		binding.method<Stategraph, &sg_name>("name", "() -> string");
		binding.method<Stategraph, &sg_graph>("graph", "() -> string");
		binding.method<Stategraph, &sg_elapsed>("elapsed", "() -> number");
		binding.method<Stategraph, &sg_tick_of>("tick_of", "(mark: string) -> number");
		binding.method<Stategraph, &sg_ticks_of>("ticks_of", "(mark: string) -> {number}");
		binding.method<Stategraph, &sg_go>("go", "(state: string) -> ()");
	}

	// --- verbs: e:play(), e:event(), e.rng ------------------------------------------------------------

	int entity_play(lua_State* L, Host& host, ecs::Entity entity)
	{
		Host::Brains& brains = HostAccess::brains(host);
		const char* clip	 = luaL_checkstring(L, 2);
		if (brains.playing == nullptr)
			luaL_error(L, "e:play: the game registered no Playing component (script::register_components)");

		const Exposed* exposed = HostAccess::binding(host).exposed(brains.playing->id);
		const Context context  = HostAccess::context(host);
		if (exposed == nullptr || !may_write(context, *exposed))
			refuse_write(L, context, *exposed, "play");

		auto* playing = static_cast<anim::Playing*>(brains.playing->get(*brains.playing, host.world().registry, entity));
		if (playing == nullptr)
			luaL_error(L, "e:play(\"%s\"): the entity has no Playing component; give its prefab Playing = {}", clip);

		playing->clip  = anim::name(clip);
		playing->since = HostAccess::now(host);
		++HostAccess::stats(host).writes;
		return 0;
	}

	int entity_event(lua_State* L, Host& host, ecs::Entity entity)
	{
		const char* name = luaL_checkstring(L, 2);
		check_context(L, host, ContextMask::All, "e:event");
		host.event(entity, name, lua_isnoneornil(L, 3) ? ecs::NO_ENTITY : check_entity(L, 3));
		return 0;
	}

	namespace
	{
		/** SplitMix64: the same numbers on every machine. */
		[[nodiscard]] u64 mix(u64 z) noexcept
		{
			z += 0x9e3779b97f4a7c15ull;
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
			return z ^ (z >> 31);
		}

		/** The entity's next roll this tick: from the tick, the entity and how many it has rolled, so a replay rolls alike. */
		[[nodiscard]] u32 roll(Host& host, ecs::Entity entity) noexcept
		{
			Host::Brains& brains = HostAccess::brains(host);
			const u32 now		 = HostAccess::now(host);
			if (brains.draws_tick != now)
			{
				brains.draws.clear();
				brains.draws_tick = now;
			}
			const u32 id = static_cast<u32>(entt::to_integral(entity));
			u32& draws	 = brains.draws[id];
			const u64 seed = (static_cast<u64>(now) << 32) ^ static_cast<u64>(id) ^ (static_cast<u64>(draws) << 48);
			++draws;
			return static_cast<u32>(mix(seed) >> 32);
		}
	}

	int rng_namecall(lua_State* L)
	{
		Host& host				 = HostAccess::of(L);
		const ecs::Entity entity = check_entity(L, 1);
		const char* method		 = lua_namecallatom(L, nullptr);
		const StringView name	 = method != nullptr ? StringView(method) : StringView();

		if (name == "range")
		{
			const f64 n = luaL_checknumber(L, 2);
			if (std::floor(n) != n || n < 1.0 || n > 4294967295.0)
				luaL_error(L, "rng:range(n) takes a whole number from 1, not %g", n);
			lua_pushnumber(L, static_cast<f64>((static_cast<u64>(roll(host, entity)) * static_cast<u64>(n)) >> 32));
			return 1;
		}
		if (name == "chance")
		{
			const f64 p = luaL_checknumber(L, 2);
			lua_pushboolean(L, static_cast<f64>(roll(host, entity)) * (1.0 / 4294967296.0) < p);
			return 1;
		}
		if (name == "float")
		{
			lua_pushnumber(L, static_cast<f64>(roll(host, entity)) * (1.0 / 4294967296.0));
			return 1;
		}
		luaL_error(L, "rng has range(n), chance(p) and float(); not '%s'", method != nullptr ? method : "?");
	}

	void install_stategraphs(lua_State* L, Host& host)
	{
		(void)host;
		lua_setlightuserdataname(L, TAG_RNG, "Rng");
	}
}
