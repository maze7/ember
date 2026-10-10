#include "internal.h"

#include <ember/core/hash.h>
#include <ember/core/logger.h>
#include <ember/physics/components.h>
#include <ember/physics/systems.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

/**
 * Stategraphs: a module's `stategraph "x" { ... }` and the `g.state "name" { ... }` statements after it, read
 * into a Graph, and the loop that steps every entity carrying a Stategraph of that graph at the graph's
 * stage. A state has handlers (enter, update, exit), named marks with handlers at them, a length and what
 * comes next, and the events it answers or ignores; the graph has what holds in every state. The state and
 * the tick it began live in the component, by the state's name, so nothing is lost to a save, a reload or a
 * late joiner, and every machine with the same scripts steps alike.
 *
 * Also here: e:event(), e:play(), e:strike(), the dice behind e.rng, and the prefab events e:event() triggers.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		constexpr StringView STATE_KEYS[] = {"every",  "enter", "update", "exit",  "timeline", "marks", "on",
											 "length", "next",	"events", "react", "ignore",   "show"};
		constexpr StringView GRAPH_KEYS[] = {"initial", "extends", "stage", "states", "on", "every", "update", "show"};

		/** A whole number at `index` from `low`; raises naming the key otherwise. */
		[[nodiscard]] u32 read_count(lua_State* L, int index, const char* graph, const char* state, const char* key,
									 f64 low)
		{
			const f64 value = declared_number(L, index, key);
			if (std::floor(value) != value || value < low || value > 4294967295.0)
				luaL_error(L, "stategraph %s, state %s: %s takes a whole number of ticks from %g, not %g", graph, state,
						   key, low, value);
			return static_cast<u32>(value);
		}

		[[nodiscard]] int ref_function(lua_State* L, int index, const char* graph, const char* state, const char* key)
		{
			if (!lua_isfunction(L, index))
				luaL_error(L, "stategraph %s, state %s: %s takes a function, not %s", graph, state, key,
						   luaL_typename(L, index));
			return lua_ref(L, index);
		}

		/** `to "state"`: what a handler that only moves on is written as. A table the loader reads; never called. */
		[[nodiscard]] bool is_to(lua_State* L, int index, String& state)
		{
			if (!lua_istable(L, index))
				return false;
			lua_rawgetfield(L, index, "__to");
			const bool found = lua_isstring(L, -1);
			if (found)
				state = String(lua_tostring(L, -1), &heap());
			lua_pop(L, 1);
			return found;
		}

		/** Marks from a { name = tick } table at `index`, sorted by tick. */
		void read_marks(lua_State* L, int index, const char* graph, const char* state, Vector<Mark>& out)
		{
			if (index < 0)
				index = lua_gettop(L) + 1 + index;
			lua_pushnil(L);
			while (lua_next(L, index) != 0)
			{
				if (!lua_isstring(L, -2) || !lua_isnumber(L, -1))
					luaL_error(L, "stategraph %s, state %s: marks is { name = tick, ... }", graph, state);
				const f64 at = lua_tonumber(L, -1);
				if (std::floor(at) != at || at < 0.0)
					luaL_error(L, "stategraph %s, state %s: mark %s is at a whole number of ticks from 0, not %g",
							   graph, state, lua_tostring(L, -2), at);
				out.push_back(
					{.at = static_cast<u32>(at), .name = intern_name(HostAccess::of(L), lua_tostring(L, -2))});
				lua_pop(L, 1);
			}
			std::sort(out.begin(), out.end(), [](const Mark& a, const Mark& b) { return a.at < b.at; });
		}

		/** How many single-letter edits turn one word into another, up to a limit: for did-you-mean. */
		[[nodiscard]] u32 edit_distance(StringView a, StringView b) noexcept
		{
			if (a.size() > 32 || b.size() > 32)
				return 99;
			u32 row[33];
			for (u32 j = 0; j <= b.size(); ++j)
				row[j] = j;
			for (u32 i = 1; i <= a.size(); ++i)
			{
				u32 diagonal = row[0];
				row[0]		 = i;
				for (u32 j = 1; j <= b.size(); ++j)
				{
					const u32 above = row[j];
					row[j]	 = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0u : 1u)});
					diagonal = above;
				}
			}
			return row[b.size()];
		}

		void read_state(lua_State* L, int table, const char* graph, State& out)
		{
			if (table < 0)
				table = lua_gettop(L) + 1 + table;
			const char* name = out.name.c_str();
			Host& host		 = HostAccess::of(L);
			int on_table	 = -1;

			lua_pushnil(L);
			while (lua_next(L, table) != 0)
			{
				if (!lua_isstring(L, -2))
					luaL_error(L, "stategraph %s, state %s: keys are names", graph, name);
				const StringView key = lua_tostring(L, -2);
				const int value		 = lua_gettop(L);

				if (key == "every")
				{
					out.every	  = read_count(L, value, graph, name, "every", 1.0);
					out.has_every = true;
				}
				else if (key == "enter")
				{
					out.enter = ref_function(L, value, graph, name, "enter");
				}
				else if (key == "update")
				{
					out.update = ref_function(L, value, graph, name, "update");
				}
				else if (key == "exit")
				{
					out.exit = ref_function(L, value, graph, name, "exit");
				}
				else if (key == "length")
				{
					// Ticks, a mark's name, or a function of the entity as it enters.
					if (lua_type(L, value) == LUA_TSTRING)
						out.length_mark = intern_name(host, lua_tostring(L, value));
					else if (lua_isfunction(L, value))
						out.length_fn = lua_ref(L, value);
					else
						out.length = read_count(L, value, graph, name, "length", 0.0);
					out.has_length = true;
				}
				else if (key == "next")
				{
					if (lua_type(L, value) == LUA_TSTRING)
						out.next_name = String(lua_tostring(L, value), &heap());
					else
						out.next_fn = ref_function(L, value, graph, name, "next");
					out.has_next = true;
				}
				else if (key == "timeline")
				{
					// The first form: { [tick] = "mark" }.
					if (!lua_istable(L, value))
						luaL_error(L, "stategraph %s, state %s: timeline is { [tick] = \"mark\", ... }", graph, name);
					lua_pushnil(L);
					while (lua_next(L, value) != 0)
					{
						if (!lua_isnumber(L, -2) || !lua_isstring(L, -1))
							luaL_error(L, "stategraph %s, state %s: timeline is { [tick] = \"mark\", ... }", graph,
									   name);
						const f64 at = lua_tonumber(L, -2);
						if (std::floor(at) != at || at < 0.0)
							luaL_error(L, "stategraph %s, state %s: a timeline tick is a whole number from 0, not %g",
									   graph, name, at);
						out.marks.push_back(
							{.at = static_cast<u32>(at), .name = intern_name(host, lua_tostring(L, -1))});
						lua_pop(L, 1);
					}
					std::sort(out.marks.begin(), out.marks.end(),
							  [](const Mark& a, const Mark& b) { return a.at < b.at; });
					out.has_marks = true;
				}
				else if (key == "marks")
				{
					// { name = tick }, or a function of the entity that gives one as it enters.
					if (lua_isfunction(L, value))
						out.marks_fn = lua_ref(L, value);
					else if (lua_istable(L, value))
						read_marks(L, value, graph, name, out.marks);
					else
						luaL_error(
							L, "stategraph %s, state %s: marks is { name = tick, ... }, or a function of the entity",
							graph, name);
					out.has_marks = true;
				}
				else if (key == "on")
				{
					if (!lua_istable(L, value))
						luaL_error(
							L, "stategraph %s, state %s: on is { mark = function(e) end, event = to \"state\", ... }",
							graph, name);
					on_table =
						lua_ref(L, value); // read below, once the marks are known whatever order the keys came in
				}
				else if (key == "events")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "stategraph %s, state %s: events is { event = \"state\", ... }", graph, name);
					lua_pushnil(L);
					while (lua_next(L, value) != 0)
					{
						if (!lua_isstring(L, -2) || !lua_isstring(L, -1))
							luaL_error(
								L,
								"stategraph %s, state %s: events is { event = \"state\", ... }; a function goes in on",
								graph, name);
						out.rules.push_back({.event		 = intern_name(host, lua_tostring(L, -2)),
											 .state_name = String(lua_tostring(L, -1), &heap())});
						lua_pop(L, 1);
					}
				}
				else if (key == "react")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "stategraph %s, state %s: react is { event = function(e, source) end, ... }",
								   graph, name);
					lua_pushnil(L);
					while (lua_next(L, value) != 0)
					{
						if (!lua_isstring(L, -2) || !lua_isfunction(L, -1))
							luaL_error(L,
									   "stategraph %s, state %s: react is { event = function(e, source) end, ... }; a "
									   "state name goes in events",
									   graph, name);
						out.rules.push_back({.event		  = intern_name(host, lua_tostring(L, -2)),
											 .react		  = lua_ref(L, -1),
											 .with_source = true});
						lua_pop(L, 1);
					}
				}
				else if (key == "ignore")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "stategraph %s, state %s: ignore is a list of event names", graph, name);
					const int count = lua_objlen(L, value);
					for (int i = 1; i <= count; ++i)
					{
						lua_rawgeti(L, value, i);
						if (!lua_isstring(L, -1))
							luaL_error(L, "stategraph %s, state %s: ignore names events", graph, name);
						out.ignored.push_back(intern_name(host, lua_tostring(L, -1)));
						lua_pop(L, 1);
					}
					out.has_ignore = true;
				}
				else if (key == "show")
				{
					String what(&heap());
					what += "stategraph ";
					what += graph;
					what += ", state ";
					what += name;
					read_show(L, value, what.c_str(), out.show);
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

			// on: a mark's handler, an event's handler, or an event that moves on with `to`.
			if (on_table >= 0)
			{
				lua_getref(L, on_table);
				const int on = lua_gettop(L);
				lua_pushnil(L);
				while (lua_next(L, on) != 0)
				{
					if (!lua_isstring(L, -2))
						luaL_error(L, "stategraph %s, state %s: on's keys are names", graph, name);
					const u64 key = intern_name(host, lua_tostring(L, -2));
					String target(&heap());
					if (is_to(L, -1, target))
					{
						out.rules.push_back({.event = key, .state_name = target});
					}
					else if (lua_isfunction(L, -1))
					{
						// A mark's handler when a mark has that name; else an event's. Marks a function makes are
						// matched to them as the state is entered, so a handler of no fixed mark is kept for both.
						bool mark = false;
						for (Mark& candidate : out.marks)
						{
							if (candidate.name != key)
								continue;
							candidate.on = lua_ref(L, -1);
							mark		 = true;
						}
						if (!mark)
							out.rules.push_back(
								{.event = key, .react = lua_ref(L, -1), .maybe_mark = out.marks_fn >= 0});
					}
					else
					{
						luaL_error(L, "stategraph %s, state %s: on.%s takes a function or to \"state\", not %s", graph,
								   name, lua_tostring(L, -2), luaL_typename(L, -1));
					}
					lua_pop(L, 1);
				}
				lua_unref(L, on_table);
				lua_pop(L, 1);
			}
		}

		/** A reference made anew to what another refers to: a copied state's own, which it lets go of itself. */
		[[nodiscard]] int reref(lua_State* L, int ref)
		{
			if (ref < 0)
				return -1;
			lua_getref(L, ref);
			const int made = lua_ref(L, -1);
			lua_pop(L, 1);
			return made;
		}

		void copy_show(lua_State* L, const Show& from, Show& to)
		{
			to.enter  = reref(L, from.enter);
			to.exit	  = reref(L, from.exit);
			to.update = reref(L, from.update);
			for (const ShowRule& rule : from.on)
				to.on.push_back({.name = rule.name, .fn = reref(L, rule.fn)});
		}

		[[nodiscard]] Rule clone_rule(lua_State* L, const Rule& from)
		{
			return {.event		 = from.event,
					.state		 = -1,
					.react		 = reref(L, from.react),
					.state_name	 = from.state_name,
					.with_source = from.with_source,
					.maybe_mark	 = from.maybe_mark};
		}

		[[nodiscard]] State clone_state(lua_State* L, const State& from)
		{
			State state;
			state.name		  = from.name;
			state.hash		  = from.hash;
			state.id		  = from.id;
			state.enter		  = reref(L, from.enter);
			state.update	  = reref(L, from.update);
			state.exit		  = reref(L, from.exit);
			state.next_fn	  = reref(L, from.next_fn);
			state.length_fn	  = reref(L, from.length_fn);
			state.marks_fn	  = reref(L, from.marks_fn);
			state.next_name	  = from.next_name;
			state.every		  = from.every;
			state.has_every	  = from.has_every;
			state.length	  = from.length;
			state.length_mark = from.length_mark;
			state.has_length  = from.has_length;
			state.has_next	  = from.has_next;
			state.has_marks	  = from.has_marks;
			state.has_ignore  = from.has_ignore;
			for (const Mark& mark : from.marks)
				state.marks.push_back({.at = mark.at, .name = mark.name, .on = reref(L, mark.on)});
			for (const Rule& rule : from.rules)
				state.rules.push_back(clone_rule(L, rule));
			state.ignored = from.ignored;
			copy_show(L, from.show, state.show);
			return state;
		}

		void release_state(lua_State* L, State& state) noexcept
		{
			for (int* ref :
				 {&state.enter, &state.update, &state.exit, &state.next_fn, &state.length_fn, &state.marks_fn})
			{
				if (*ref >= 0)
					lua_unref(L, *ref);
				*ref = -1;
			}
			for (const Mark& mark : state.marks)
				if (mark.on >= 0)
					lua_unref(L, mark.on);
			for (const Rule& rule : state.rules)
				if (rule.react >= 0)
					lua_unref(L, rule.react);
			state.marks.clear();
			state.rules.clear();
			release_show(L, state.show);
		}

		/** A state's keys over an inherited one: what `over` says replaces, key by key; the rest stays the base's. */
		void merge_state(lua_State* L, State& base, State& over)
		{
			const auto take = [&](int& into, int& from)
			{
				if (from < 0)
					return;
				if (into >= 0)
					lua_unref(L, into);
				into = from;
				from = -1;
			};
			const auto drop = [&](int& ref)
			{
				if (ref >= 0)
					lua_unref(L, ref);
				ref = -1;
			};

			take(base.enter, over.enter);
			take(base.update, over.update);
			take(base.exit, over.exit);
			if (over.has_every)
			{
				base.every	   = over.every;
				base.has_every = true;
			}
			if (over.has_length)
			{
				drop(base.length_fn);
				take(base.length_fn, over.length_fn);
				base.length		 = over.length;
				base.length_mark = over.length_mark;
				base.has_length	 = true;
			}
			if (over.has_next)
			{
				drop(base.next_fn);
				take(base.next_fn, over.next_fn);
				base.next_name = over.next_name;
				base.has_next  = true;
			}
			if (over.has_marks)
			{
				for (const Mark& mark : base.marks)
					if (mark.on >= 0)
						lua_unref(L, mark.on);
				base.marks = std::move(over.marks);
				over.marks.clear();
				drop(base.marks_fn);
				take(base.marks_fn, over.marks_fn);
				base.has_marks = true;
			}
			else
			{
				// The base's marks with the handlers this state gives them.
				for (Mark& mark : over.marks)
					for (Mark& known : base.marks)
						if (known.name == mark.name && mark.on >= 0)
						{
							drop(known.on);
							known.on = reref(L, mark.on);
						}
			}
			for (Rule& rule : over.rules)
			{
				std::erase_if(base.rules,
							  [&](Rule& known)
							  {
								  if (known.event != rule.event)
									  return false;
								  if (known.react >= 0)
									  lua_unref(L, known.react);
								  return true;
							  });
				base.rules.push_back(std::move(rule));
			}
			over.rules.clear();
			if (over.has_ignore)
			{
				base.ignored	= std::move(over.ignored);
				base.has_ignore = true;
			}

			// The show, handler by handler too.
			take(base.show.enter, over.show.enter);
			take(base.show.exit, over.show.exit);
			take(base.show.update, over.show.update);
			for (ShowRule& rule : over.show.on)
			{
				std::erase_if(base.show.on,
							  [&](ShowRule& known)
							  {
								  if (known.name != rule.name)
									  return false;
								  if (known.fn >= 0)
									  lua_unref(L, known.fn);
								  return true;
							  });
				base.show.on.push_back(rule);
			}
			over.show.on.clear();
		}

		/** g.state "name" { ... }: into the graph its module is declaring. Declared twice, the later says more. */
		void add_state(lua_State* L, Graph& graph, const char* name, int table)
		{
			State state;
			state.name = String(name, &heap());
			state.hash = hash_text(state.name);
			state.id   = static_cast<StateId>(state.hash);
			read_state(L, table, graph.name.c_str(), state);

			for (State& known : graph.states)
			{
				if (known.id != state.id)
					continue;
				merge_state(L, known, state);
				release_state(L, state);
				return;
			}
			graph.states.push_back(std::move(state));
		}
	}

	void read_graph(lua_State* L, Host& host, int index, Graph& out)
	{
		if (index < 0)
			index = lua_gettop(L) + 1 + index;
		const char* graph = out.name.c_str();
		bool stage_given  = false;

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
				out.initial_name = String(lua_tostring(L, value), &heap());
			}
			else if (key == "extends")
			{
				if (!lua_isstring(L, value))
					luaL_error(L, "stategraph %s: extends names a stategraph", graph);
				out.extends = String(lua_tostring(L, value), &heap());
			}
			else if (key == "stage")
			{
				if (!lua_isstring(L, value))
					luaL_error(L, "stategraph %s: stage names a stage of the tick", graph);
				const StageName* stage = HostAccess::binding(host).find_stage(lua_tostring(L, value));
				if (stage == nullptr)
					luaL_error(L, "stategraph %s: no stage named '%s'", graph, lua_tostring(L, value));
				out.stage	= stage->index;
				stage_given = true;
			}
			else if (key == "states")
			{
				if (!lua_istable(L, value))
					luaL_error(L, "stategraph %s: states is { name = { ... }, ... }", graph);
				lua_pushnil(L);
				while (lua_next(L, value) != 0)
				{
					if (!lua_isstring(L, -2) || !lua_istable(L, -1))
						luaL_error(L, "stategraph %s: a state is a name and a table", graph);
					add_state(L, out, lua_tostring(L, -2), lua_gettop(L));
					lua_pop(L, 1);
				}
			}
			else if (key == "on" || key == "update" || key == "every")
			{
				// What holds in every state: read as a state of its own, and kept on the graph.
				lua_createtable(L, 0, 1);
				lua_pushvalue(L, value);
				lua_setfield(L, -2, key.data());
				State shared;
				shared.name = String("*", &heap());
				read_state(L, lua_gettop(L), graph, shared);
				lua_pop(L, 1);
				if (key == "on")
				{
					for (Rule& rule : shared.rules)
						out.rules.push_back(std::move(rule));
					shared.rules.clear();
				}
				else if (key == "update")
				{
					out.update = std::exchange(shared.update, -1);
				}
				else
				{
					out.every = shared.every;
				}
				release_state(L, shared);
			}
			else if (key == "show")
			{
				String what("stategraph ", &heap());
				what += graph;
				read_show(L, value, what.c_str(), out.show);
			}
			else
			{
				String keys(&heap());
				for (const StringView known : GRAPH_KEYS)
				{
					if (!keys.empty())
						keys += ", ";
					keys += known;
				}
				luaL_error(L, "stategraph %s: unknown key '%s'; a stategraph has %s", graph, key.data(), keys.c_str());
			}
			lua_pop(L, 1);
		}

		// The stage: the one asked for, or the first of the tick.
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
	}

	bool finish_graph(lua_State* L, Host& host, Graph& out, Span<Graph> siblings, String& why, Vector<String>& warnings)
	{
		Host::Brains& brains = HostAccess::brains(host);
		const auto fail		 = [&](String message)
		{
			why = std::move(message);
			return false;
		};
		const auto in_state = [&](const State* state)
		{
			String where("stategraph ", &heap());
			where += out.name;
			if (state != nullptr)
			{
				where += ", state ";
				where += state->name;
			}
			return where;
		};

		// Another graph's states first, when this one extends it: one this module declared earlier, or one loaded.
		if (!out.extends.empty())
		{
			const Graph* base = nullptr;
			for (const Graph& candidate : siblings)
				if (candidate.name == out.extends && &candidate != &out && candidate.finished)
					base = &candidate;
			if (base == nullptr)
				base = brains.graph(graph_id(out.extends));
			if (base == nullptr || !base->finished)
				return fail(in_state(nullptr) + " extends '" + out.extends +
							"', which is not a stategraph loaded before it");

			Vector<State> own(std::move(out.states));
			out.states.clear();
			for (const State& state : base->states)
				out.states.push_back(clone_state(L, state));
			for (State& state : own)
			{
				bool merged = false;
				for (State& known : out.states)
				{
					if (known.id != state.id)
						continue;
					merge_state(L, known, state);
					release_state(L, state);
					merged = true;
					break;
				}
				if (!merged)
					out.states.push_back(std::move(state));
			}
			if (out.initial_name.empty() && base->initial >= 0)
				out.initial_name = base->states[static_cast<size_t>(base->initial)].name;

			// What holds in every state: the base's, under what this graph says.
			for (const Rule& rule : base->rules)
			{
				bool given = false;
				for (const Rule& own_rule : out.rules)
					given = given || own_rule.event == rule.event;
				if (!given)
					out.rules.push_back(clone_rule(L, rule));
			}
			if (out.update < 0)
			{
				out.update = reref(L, base->update);
				out.every  = base->every;
			}
			if (out.show.empty())
				copy_show(L, base->show, out.show);
		}

		if (out.states.empty())
			return fail(in_state(nullptr) + ": no states");
		if (out.initial_name.empty())
			return fail(in_state(nullptr) + ": no initial state");

		// States sorted by name, each hashing apart from the rest.
		std::sort(out.states.begin(), out.states.end(), [](const State& a, const State& b) { return a.name < b.name; });
		for (size_t i = 0; i + 1 < out.states.size(); ++i)
			for (size_t j = i + 1; j < out.states.size(); ++j)
				if (out.states[i].id == out.states[j].id)
					return fail(in_state(nullptr) + ": states " + out.states[i].name + " and " + out.states[j].name +
								" hash alike; rename one");

		out.initial = out.find(out.initial_name);
		if (out.initial < 0)
			return fail(in_state(nullptr) + ": initial state '" + out.initial_name + "' is not among the states");

		const auto resolve_rules = [&](Vector<Rule>& rules, const State* state) -> bool
		{
			for (Rule& rule : rules)
			{
				if (rule.state_name.empty())
					continue;
				rule.state = out.find(rule.state_name);
				if (rule.state < 0)
					return fail(in_state(state) + ": an event moves to '" + rule.state_name +
								"', which is not a state");
			}
			for (size_t i = 0; i + 1 < rules.size(); ++i)
				for (size_t j = i + 1; j < rules.size(); ++j)
					if (rules[i].event == rules[j].event)
						return fail(in_state(state) + ": an event is answered twice");
			return true;
		};
		if (!resolve_rules(out.rules, nullptr))
			return false;

		for (State& state : out.states)
		{
			if (state.has_next && !state.has_length)
				return fail(in_state(&state) +
							": next without length; next is where the state goes when its length is up");
			if (state.has_length && !state.has_next)
				return fail(in_state(&state) + ": length without next");
			if (state.has_next && state.next_fn < 0)
			{
				state.next_state = out.find(state.next_name);
				if (state.next_state < 0)
					return fail(in_state(&state) + ": next names '" + state.next_name + "', which is not a state");
			}
			if (state.length_mark != 0 && state.marks_fn < 0)
			{
				bool found = false;
				for (const Mark& mark : state.marks)
				{
					if (mark.name != state.length_mark)
						continue;
					state.length = mark.at;
					found		 = true;
				}
				if (!found)
					return fail(in_state(&state) + ": length names a mark the state has not");
			}
			if (!resolve_rules(state.rules, &state))
				return false;

			// An `on` key that is no mark, but nearly one: a mark misspelt, which would wait for an event instead.
			if (state.marks_fn < 0)
			{
				for (const Rule& rule : state.rules)
				{
					if (rule.react < 0 || rule.with_source)
						continue;
					const StringView said = name_text(host.state(), rule.event);
					for (const Mark& mark : state.marks)
					{
						const StringView mark_name = name_text(host.state(), mark.name);
						if (!said.empty() && !mark_name.empty() && edit_distance(said, mark_name) <= 2)
						{
							warnings.push_back(in_state(&state) + ": on." + String(said, &heap()) +
											   " is no mark, so it waits for an event of that name; did you mean '" +
											   String(mark_name, &heap()) + "'?");
							break;
						}
					}
				}
			}
		}

		out.finished = true;
		return true;
	}

	void release_graph(lua_State* L, Graph& graph) noexcept
	{
		for (State& state : graph.states)
			release_state(L, state);
		for (const Rule& rule : graph.rules)
			if (rule.react >= 0)
				lua_unref(L, rule.react);
		if (graph.update >= 0)
			lua_unref(L, graph.update);
		graph.update = -1;
		release_show(L, graph.show);
		graph.states.clear();
		graph.rules.clear();
	}

	// --- marks an entity's state has ------------------------------------------------------------------

	namespace
	{
		[[nodiscard]] u64 marks_key(ecs::Entity entity, u32 slot) noexcept
		{
			return (static_cast<u64>(entt::to_integral(entity)) << 8) | slot;
		}

		/**
		 * The marks a state's function makes for an entity, made on `L` as it enters and kept by (entity, slot,
		 * state, since), so a correction or a reload that puts the state back makes them again from the entity
		 * as it then is. False when the function failed, which is reported.
		 */
		[[nodiscard]] bool make_marks(lua_State* L, Host& host, const Graph& graph, const State& state,
									  ecs::Entity entity, u32 slot, const Stategraph::Slot& sg)
		{
			Host::Brains& brains = HostAccess::brains(host);
			const int top		 = lua_gettop(L);
			lua_getref(L, state.marks_fn);
			push_entity(L, entity);
			const Context outer = HostAccess::context(host);
			HostAccess::begin_call(host, graph.context);
			const int status = lua_pcall(L, 1, 1, 0);
			HostAccess::set_context(host, outer);
			++HostAccess::stats(host).calls;
			if (status != LUA_OK || !lua_istable(L, -1))
			{
				if (status != LUA_OK)
					HostAccess::report_lua(host, HostAccess::path_of(host, graph.module), lua_tostring(L, -1));
				else
					HostAccess::report(host, HostAccess::path_of(host, graph.module), 0, Severity::Error,
									   "stategraph " + graph.name + ", state " + state.name +
										   ": marks must give { name = tick, ... }");
				lua_settop(L, top);
				++HostAccess::stats(host).errors;
				return false;
			}

			EntityMarks& made = brains.entity_marks[marks_key(entity, slot)];
			made.state		  = sg.state;
			made.since		  = sg.since;
			made.marks.clear();
			lua_pushnil(L);
			while (lua_next(L, -2) != 0)
			{
				if (lua_isstring(L, -2) && lua_isnumber(L, -1) && lua_tonumber(L, -1) >= 0.0)
				{
					const u64 name = intern_name(host, lua_tostring(L, -2));
					Mark mark{.at = static_cast<u32>(lua_tonumber(L, -1)), .name = name, .on = -1};
					for (const Rule& rule : state.rules)
						if (rule.maybe_mark && rule.event == name)
							mark.on = rule.react;
					made.marks.push_back(mark);
				}
				lua_pop(L, 1);
			}
			std::sort(made.marks.begin(), made.marks.end(), [](const Mark& a, const Mark& b) { return a.at < b.at; });
			lua_settop(L, top);
			return true;
		}

		/** An entity's marks in its state: the state's own, or the ones its function makes, made when missing or stale.
		 */
		[[nodiscard]] const Vector<Mark>* marks_of(lua_State* L, Host& host, Graph& graph, const State& state,
												   ecs::Entity entity, u32 slot, const Stategraph::Slot& sg)
		{
			if (state.marks_fn < 0)
				return &state.marks;
			Host::Brains& brains = HostAccess::brains(host);
			const auto found	 = brains.entity_marks.find(marks_key(entity, slot));
			if (found != brains.entity_marks.end() && found->second.since == sg.since &&
				found->second.state == sg.state)
				return &found->second.marks;
			if (!make_marks(L, host, graph, state, entity, slot, sg))
			{
				graph.disabled = true;
				return nullptr;
			}
			return &brains.entity_marks[marks_key(entity, slot)].marks;
		}

		[[nodiscard]] bool mark_tick(const Vector<Mark>& marks, u64 name, u32& at) noexcept
		{
			for (const Mark& mark : marks)
			{
				if (mark.name == name)
				{
					at = mark.at;
					return true;
				}
			}
			return false;
		}
	}

	// --- the step -----------------------------------------------------------------------------------

	namespace
	{
		/**
		 * Calls one of a graph's functions on its module's thread with the entity, and the event when there is
		 * one: as { name, tick, by, n }, or, for the first form's react handlers, as its source entity. A string
		 * it returns names the next state, in `target`. False on an error, which turns the graph off until its
		 * module reloads.
		 */
		[[nodiscard]] bool call(Host& host, Graph& graph, int ref, ecs::Entity entity, const Event* event,
								bool with_source, i16& target, const char* what, const State& state)
		{
			lua_State* thread = HostAccess::thread_of(host, graph.module);
			lua_settop(thread, 0);
			lua_getref(thread, ref);
			push_entity(thread, entity);
			int args = 1;
			if (event != nullptr)
			{
				if (with_source)
				{
					if (event->source == ecs::NO_ENTITY || !host.world().registry.valid(event->source))
						lua_pushnil(thread);
					else
						push_entity(thread, event->source);
				}
				else
				{
					push_event_table(thread, host, *event);
				}
				args = 2;
			}

			HostAccess::begin_call(host, graph.context);
			const int status = lua_pcall(thread, args, 1, 0);
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

		/** A length from a function of the entity, as it enters. */
		[[nodiscard]] bool call_length(Host& host, Graph& graph, const State& state, ecs::Entity entity, u32& out)
		{
			lua_State* thread = HostAccess::thread_of(host, graph.module);
			lua_settop(thread, 0);
			lua_getref(thread, state.length_fn);
			push_entity(thread, entity);
			HostAccess::begin_call(host, graph.context);
			const int status = lua_pcall(thread, 1, 1, 0);
			HostAccess::end_call(host);
			++HostAccess::stats(host).calls;
			const f64 value = status == LUA_OK && lua_isnumber(thread, -1) ? lua_tonumber(thread, -1) : -1.0;
			if (status != LUA_OK)
				HostAccess::report_lua(host, HostAccess::path_of(host, graph.module), lua_tostring(thread, -1));
			else if (value < 0.0 || std::floor(value) != value)
				HostAccess::report(host, HostAccess::path_of(host, graph.module), 0, Severity::Error,
								   "stategraph " + graph.name + ", state " + state.name +
									   ": length must give a whole number of ticks");
			lua_settop(thread, 0);
			if (status != LUA_OK || value < 0.0 || std::floor(value) != value)
			{
				graph.disabled = true;
				++HostAccess::stats(host).errors;
				return false;
			}
			out = static_cast<u32>(value);
			return true;
		}

		/** The length a state has for an entity: its ticks, its mark's tick, or its function's answer. */
		[[nodiscard]] bool length_of(Host& host, Graph& graph, const State& state, ecs::Entity entity, u32 slot,
									 const Stategraph::Slot& sg, u32& out)
		{
			if (state.length_fn >= 0)
				return call_length(host, graph, state, entity, out);
			if (state.length_mark != 0 && state.marks_fn >= 0)
			{
				const Vector<Mark>* marks =
					marks_of(HostAccess::thread_of(host, graph.module), host, graph, state, entity, slot, sg);
				if (marks == nullptr)
					return false;
				if (!mark_tick(*marks, state.length_mark, out))
				{
					HostAccess::report(host, HostAccess::path_of(host, graph.module), 0, Severity::Error,
									   "stategraph " + graph.name + ", state " + state.name +
										   ": length names a mark its marks lack");
					graph.disabled = true;
					return false;
				}
				return true;
			}
			out = state.length;
			return true;
		}

		/** Runs the handlers of every mark at `at`; false on an error. */
		[[nodiscard]] bool fire_marks(Host& host, Graph& graph, const State& state, ecs::Entity entity, u32 slot,
									  const Stategraph::Slot& sg, u32 at)
		{
			Host::Brains& brains = HostAccess::brains(host);
			const Vector<Mark>* marks =
				marks_of(HostAccess::thread_of(host, graph.module), host, graph, state, entity, slot, sg);
			if (marks == nullptr)
				return false;
			for (const Mark& mark : *marks)
			{
				if (mark.at != at || mark.on < 0)
					continue;
				i16 ignored = -1;
				if (!call(host, graph, mark.on, entity, nullptr, false, ignored, "a mark handler", state))
					return false;
				if (brains.went)
					return true;
			}
			return true;
		}

		/** The rule a state answers an event with: its own, or the graph's, unless the state ignores the event. */
		[[nodiscard]] const Rule* rule_for(const Graph& graph, const State& state, u64 event) noexcept
		{
			if (const Rule* own = state.rule(event); own != nullptr)
				return own;
			for (const u64 ignored : state.ignored)
				if (ignored == event)
					return nullptr;
			for (const Rule& rule : graph.rules)
				if (rule.event == event)
					return &rule;
			return nullptr;
		}

		/** A strike ends: the hitbox goes back to what it hit before. */
		void end_strike(ecs::World& world, ecs::Entity entity, Strike& strike) noexcept
		{
			if (auto* hitbox = world.registry.try_get<physics::Hitbox>(entity))
				hitbox->hits.bits = strike.restore;
			strike = {};
		}

		/** Whether the entity's hitbox began to touch something in the latest collide: what ends a strike once. */
		[[nodiscard]] bool struck_something(const ecs::World& world, ecs::Entity entity) noexcept
		{
			const physics::Hits* hits = world.registry.ctx().find<physics::Hits>();
			if (hits == nullptr)
				return false;
			for (const physics::Hit& hit : *hits)
				if (hit.hitbox == entity && hit.began)
					return true;
			return false;
		}

		/** One tick of one slot's graph: the events it has not seen, then its state's handlers, through any
		 * transitions. */
		void step(Host& host, Graph& graph, ecs::Entity entity, u32 slot, Stategraph::Slot& sg, u32 now,
				  const Vector<const Event*>& events)
		{
			Host::Brains& brains = HostAccess::brains(host);
			ecs::World& world	 = host.world();
			i16 target			 = -1;
			i16 current			 = graph.index_of(sg.state);

			if (sg.state != NO_STATE && current < 0)
				sg.state = NO_STATE; // the graph lost this state in a reload: start again
			if (current < 0)
				for (const Event* event : events)
					if (event->name == FORCE_EVENT && graph.index_of(event->forced) >= 0)
						target = graph.index_of(event->forced);

			// A strike of this slot's that has run its course, hit what it was to hit once, or outlived its state.
			Strike* strike = world.registry.try_get<Strike>(entity);
			if (strike != nullptr && strike->active && strike->slot == slot &&
				(now >= strike->until || strike->since != sg.since ||
				 (strike->once && struck_something(world, entity))))
				end_strike(world, entity, *strike);

			// The events, each handled by the state it is in as it is read; one already moving it on drops the
			// rest, as a state that is left hears nothing more.
			if (current >= 0)
			{
				for (const Event* event : events)
				{
					if (target >= 0 || brains.went)
						break;
					if (event->name == FORCE_EVENT)
					{
						// An inspector's: straight into the state it names, whatever this one answers or ignores.
						if (const i16 forced = graph.index_of(event->forced); forced >= 0)
							target = forced;
						continue;
					}
					const State& state = graph.states[static_cast<size_t>(current)];
					const Rule* rule   = rule_for(graph, state, event->name);
					if (rule == nullptr)
						continue;
					if (rule->state >= 0)
						target = rule->state;
					else if (rule->react >= 0 && !call(host, graph, rule->react, entity, event, rule->with_source,
													   target, "a handler", state))
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
					current		= graph.index_of(sg.state);
					++transitions;
				}
				if (sg.state == NO_STATE || current < 0)
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
					if (current >= 0)
					{
						const State& leaving = graph.states[static_cast<size_t>(current)];
						if (leaving.exit >= 0)
						{
							i16 ignored = -1;
							if (!call(host, graph, leaving.exit, entity, nullptr, false, ignored, "exit", leaving))
								return;
						}
						// A strike lasts no longer than its state.
						if (strike != nullptr && strike->active && strike->slot == slot)
							end_strike(world, entity, *strike);
						record_transition(host, entity, slot, leaving.id, graph.states[static_cast<size_t>(target)].id,
										  now);
					}
					else
					{
						record_transition(host, entity, slot, NO_STATE, graph.states[static_cast<size_t>(target)].id,
										  now);
					}
					current	 = target;
					sg.state = graph.states[static_cast<size_t>(current)].id;
					sg.since = now;
					target	 = -1;
					fresh	 = true;
					++HostAccess::stats(host).transitions;
				}

				const State& state = graph.states[static_cast<size_t>(current)];
				const u32 elapsed  = now >= sg.since ? now - sg.since : 0;

				if (fresh)
				{
					fresh = false;
					if (state.enter >= 0)
					{
						if (!call(host, graph, state.enter, entity, nullptr, false, target, "enter", state))
							return;
						if (brains.went || target >= 0)
							continue;
					}
					if (!fire_marks(host, graph, state, entity, slot, sg, 0))
						return;
					if (brains.went)
						continue;
				}
				else
				{
					u32 length = 0;
					if (state.has_length && !length_of(host, graph, state, entity, slot, sg, length))
						return;
					if (state.has_length && elapsed >= length)
					{
						if (state.next_state >= 0)
							target = state.next_state;
						else if (state.next_fn >= 0 &&
								 !call(host, graph, state.next_fn, entity, nullptr, false, target, "next", state))
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
					if (!fire_marks(host, graph, state, entity, slot, sg, elapsed))
						return;
					if (brains.went)
						continue;
				}

				// The state's update at its cadence, then the graph's, in whatever state it is in.
				if (state.update >= 0 && (state.every == 0 || elapsed % state.every == 0))
				{
					if (!call(host, graph, state.update, entity, nullptr, false, target, "update", state))
						return;
					if (brains.went || target >= 0)
						continue;
				}
				if (graph.update >= 0 && (graph.every == 0 || elapsed % graph.every == 0))
				{
					if (!call(host, graph, graph.update, entity, nullptr, false, target, "the graph's update", state))
						return;
					if (brains.went || target >= 0)
						continue;
				}
				break;
			}
		}

		/** A prefab event's groups onto an entity, through the commands: the removed groups' components back to the
		 * prefab's or gone, the added groups' in. */
		void apply_prefab_event(Host& host, const Event& event)
		{
			ecs::World& world		= host.world();
			Host::Brains& brains	= HostAccess::brains(host);
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

	void push_event_table(lua_State* L, Host& host, const Event& event)
	{
		lua_createtable(L, 0, 4);
		const StringView text = name_text(L, event.name);
		if (text.empty())
			lua_pushfstring(L, "%08x", static_cast<unsigned>(event.name));
		else
			lua_pushlstring(L, text.data(), text.size());
		lua_setfield(L, -2, "name");
		lua_pushnumber(L, static_cast<f64>(event.tick));
		lua_setfield(L, -2, "tick");
		if (event.source != ecs::NO_ENTITY && host.world().registry.valid(event.source))
		{
			push_entity(L, event.source);
			lua_setfield(L, -2, "by");
		}
		lua_pushnumber(L, static_cast<f64>(event.value));
		lua_setfield(L, -2, "n");
	}

	void record_transition(Host& host, ecs::Entity entity, u32 slot, StateId from, StateId to, u32 tick) noexcept
	{
		// Kept only while an inspector watches: its entity's last few.
		Host::Brains& brains = HostAccess::brains(host);
		if (brains.watching != entity)
			return;
		Transition& made	 = brains.history[brains.history_next];
		made				 = {.slot = static_cast<u8>(slot), .from = from, .to = to, .tick = tick};
		brains.history_next	 = (brains.history_next + 1) % Host::Brains::HISTORY;
		brains.history_count = std::min<u32>(brains.history_count + 1, Host::Brains::HISTORY);
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

		// Now and then, the marks of entities that have gone.
		if (now - brains.marks_swept >= 256)
		{
			brains.marks_swept = now;
			std::erase_if(brains.entity_marks, [&](const auto& entry)
						  { return !world.registry.valid(static_cast<ecs::Entity>(entry.first >> 8)); });
		}

		if (brains.stategraph == nullptr || brains.graphs.empty())
			return;
		const entt::sparse_set* storage = world.registry.storage(brains.stategraph->type);
		if (storage == nullptr)
			return;

		Vector<size_t> theirs(&heap());
		Vector<const Event*> unseen(&heap());
		for (u32 i = static_cast<u32>(storage->size()); i-- > 0;)
		{
			const ecs::Entity entity = (*storage)[i];
			auto* sg = static_cast<Stategraph*>(brains.stategraph->get(*brains.stategraph, world.registry, entity));
			if (sg == nullptr || !world.registry.all_of<ecs::Simulated>(entity))
				continue;

			theirs.clear();
			for (size_t k = 0; k < brains.events.size(); ++k)
				if (brains.events[k].target == entity)
					theirs.push_back(k);

			for (u32 slot = 0; slot < Stategraph::SLOTS; ++slot)
			{
				Graph* graph = brains.graph(sg->slots[slot].graph);
				if (graph == nullptr || graph->disabled || graph->stage != stage ||
					!runs_in(graph->context, world.role()))
					continue;

				// Each event once a slot, answered or not: what a handler raises waits for the next point.
				const u8 bit = static_cast<u8>(1u << slot);
				unseen.clear();
				for (const size_t k : theirs)
					if ((brains.events[k].delivered & bit) == 0)
						unseen.push_back(&brains.events[k]);

				brains.stepping		 = entity;
				brains.stepping_slot = slot;
				brains.went			 = false;
				step(host, *graph, entity, slot, sg->slots[slot], now, unseen);

				for (const size_t k : theirs)
					brains.events[k].delivered |= bit;
			}
		}
		brains.stepping = ecs::NO_ENTITY;
	}

	// --- the Stategraph component's methods -----------------------------------------------------------

	namespace
	{
		/**
		 * The slot a method means: the one being stepped, when the entity is the one stepping; else the
		 * first. A trailing number names another, counted from 1.
		 */
		[[nodiscard]] u32 slot_index(lua_State* L, ecs::Entity self, int arg)
		{
			const Host::Brains& brains = HostAccess::brains(HostAccess::of(L));
			u32 slot				   = brains.running && brains.stepping == self ? brains.stepping_slot : 0;
			if (!lua_isnoneornil(L, arg))
			{
				const f64 given = luaL_checknumber(L, arg);
				if (given < 1.0 || given > static_cast<f64>(Stategraph::SLOTS) || std::floor(given) != given)
					luaL_error(L, "Stategraph: a slot is 1 or 2, not %g", given);
				slot = static_cast<u32>(given) - 1;
			}
			return slot;
		}

		[[nodiscard]] Graph* graph_of(lua_State* L, const Stategraph::Slot& sg) noexcept
		{
			return HostAccess::brains(HostAccess::of(L)).graph(sg.graph);
		}

		[[nodiscard]] const State* state_in(const Graph* graph, const Stategraph::Slot& sg) noexcept
		{
			const i16 index = graph != nullptr ? graph->index_of(sg.state) : -1;
			return index < 0 ? nullptr : &graph->states[static_cast<size_t>(index)];
		}

		int sg_name(lua_State* L, Stategraph& all, ecs::Entity self)
		{
			const Stategraph::Slot& sg = all.slots[slot_index(L, self, 2)];
			const State* state		   = state_in(graph_of(L, sg), sg);
			if (state == nullptr)
				lua_pushstring(L, "");
			else
				lua_pushlstring(L, state->name.data(), state->name.size());
			return 1;
		}

		int sg_graph(lua_State* L, Stategraph& all, ecs::Entity self)
		{
			const Graph* graph = graph_of(L, all.slots[slot_index(L, self, 2)]);
			if (graph == nullptr)
				lua_pushstring(L, "");
			else
				lua_pushlstring(L, graph->name.data(), graph->name.size());
			return 1;
		}

		[[nodiscard]] u32 elapsed_of(lua_State* L, const Stategraph::Slot& sg) noexcept
		{
			const u32 now = HostAccess::now(HostAccess::of(L));
			return sg.state == NO_STATE || now < sg.since ? 0 : now - sg.since;
		}

		int sg_elapsed(lua_State* L, Stategraph& all, ecs::Entity self)
		{
			lua_pushnumber(L, static_cast<f64>(elapsed_of(L, all.slots[slot_index(L, self, 2)])));
			return 1;
		}

		/** A mark's tick from its state's start, in the entity's slot; raises for a mark it has not. */
		[[nodiscard]] u32 mark_at(lua_State* L, Stategraph& all, ecs::Entity self, int mark_arg, int slot_arg,
								  const char* what)
		{
			const char* mark		   = luaL_checkstring(L, mark_arg);
			const u32 slot			   = slot_index(L, self, slot_arg);
			const Stategraph::Slot& sg = all.slots[slot];
			Graph* graph			   = graph_of(L, sg);
			if (graph == nullptr)
				luaL_error(L, "Stategraph:%s: no loaded stategraph drives this entity", what);
			const State* state = state_in(graph, sg);
			if (state == nullptr)
				luaL_error(L, "Stategraph:%s: not in a state yet", what);
			const Vector<Mark>* marks = marks_of(L, HostAccess::of(L), *graph, *state, self, slot, sg);
			u32 at					  = 0;
			if (marks == nullptr || !mark_tick(*marks, hash_text(mark), at))
				luaL_error(L, "Stategraph:%s: state %s has no mark '%s'", what, state->name.c_str(), mark);
			return at;
		}

		int sg_at(lua_State* L, Stategraph& all, ecs::Entity self)
		{
			lua_pushnumber(L, static_cast<f64>(mark_at(L, all, self, 2, 3, "at")));
			return 1;
		}

		int sg_tick_of(lua_State* L, Stategraph& all, ecs::Entity self)
		{
			const u32 at = mark_at(L, all, self, 2, 3, "tick_of");
			lua_pushnumber(L, static_cast<f64>(all.slots[slot_index(L, self, 3)].since) + static_cast<f64>(at));
			return 1;
		}

		int sg_ticks_of(lua_State* L, Stategraph& all, ecs::Entity self)
		{
			const char* mark		   = luaL_checkstring(L, 2);
			const u32 slot			   = slot_index(L, self, 3);
			const Stategraph::Slot& sg = all.slots[slot];
			Graph* graph			   = graph_of(L, sg);
			const State* state		   = state_in(graph, sg);
			if (state == nullptr)
				luaL_error(L, "Stategraph:ticks_of: not in a state yet");
			const Vector<Mark>* marks = marks_of(L, HostAccess::of(L), *graph, *state, self, slot, sg);
			const u64 name			  = hash_text(mark);
			lua_newtable(L);
			int count = 0;
			if (marks != nullptr)
			{
				for (const Mark& candidate : *marks)
				{
					if (candidate.name != name)
						continue;
					lua_pushnumber(L, static_cast<f64>(sg.since) + static_cast<f64>(candidate.at));
					lua_rawseti(L, -2, ++count);
				}
			}
			return 1;
		}

		int sg_past(lua_State* L, Stategraph& all, ecs::Entity self)
		{
			const u32 at = mark_at(L, all, self, 2, 3, "past");
			lua_pushboolean(L, elapsed_of(L, all.slots[slot_index(L, self, 3)]) >= at);
			return 1;
		}

		int sg_between(lua_State* L, Stategraph& all, ecs::Entity self)
		{
			const u32 from	  = mark_at(L, all, self, 2, 4, "between");
			const u32 to	  = mark_at(L, all, self, 3, 4, "between");
			const u32 elapsed = elapsed_of(L, all.slots[slot_index(L, self, 4)]);
			lua_pushboolean(L, elapsed >= from && elapsed < to);
			return 1;
		}

		int sg_go(lua_State* L, Stategraph& all, ecs::Entity self)
		{
			Host& host			 = HostAccess::of(L);
			Host::Brains& brains = HostAccess::brains(host);
			const char* name	 = luaL_checkstring(L, 2);
			if (!brains.running || brains.stepping != self)
				luaL_error(L,
						   "Stategraph:go is for the graph's own handlers; from anywhere else, raise an event: "
						   "e:event(\"%s\")",
						   name);

			Stategraph::Slot& sg = all.slots[brains.stepping_slot];
			const Graph* graph	 = graph_of(L, sg);
			const State* state	 = state_in(graph, sg);
			if (state == nullptr)
				luaL_error(L, "Stategraph:go: not in a state yet");
			const i16 target = graph->find(name);
			if (target < 0)
				luaL_error(L, "Stategraph:go: '%s' is not a state of %s", name, graph->name.c_str());

			// Its exit runs here, on the caller's stack; the step enters the new state when the handler returns.
			if (state->exit >= 0)
			{
				lua_getref(L, state->exit);
				push_entity(L, self);
				lua_call(L, 1, 0);
			}
			if (Strike* strike = host.world().registry.try_get<Strike>(self);
				strike != nullptr && strike->active && strike->slot == brains.stepping_slot)
				end_strike(host.world(), self, *strike);
			record_transition(host, self, brains.stepping_slot, sg.state, graph->states[static_cast<size_t>(target)].id,
							  HostAccess::now(host));
			sg.state	= graph->states[static_cast<size_t>(target)].id;
			sg.since	= HostAccess::now(host);
			brains.went = true;
			++HostAccess::stats(host).transitions;
			return 0;
		}
	}

	void expose_stategraph(Binding& binding) noexcept
	{
		binding.expose<Stategraph>();
		binding.method<Stategraph, &sg_name>("name", "(slot: number?) -> string");
		binding.method<Stategraph, &sg_graph>("graph", "(slot: number?) -> string");
		binding.method<Stategraph, &sg_elapsed>("elapsed", "(slot: number?) -> number");
		binding.method<Stategraph, &sg_at>("at", "(mark: string, slot: number?) -> number");
		binding.method<Stategraph, &sg_tick_of>("tick_of", "(mark: string, slot: number?) -> number");
		binding.method<Stategraph, &sg_ticks_of>("ticks_of", "(mark: string, slot: number?) -> {number}");
		binding.method<Stategraph, &sg_past>("past", "(mark: string, slot: number?) -> boolean");
		binding.method<Stategraph, &sg_between>("between", "(from: string, to: string, slot: number?) -> boolean");
		binding.method<Stategraph, &sg_go>("go", "(state: string) -> ()");
	}

	bool force_state(Host& host, ecs::Entity entity, u32 slot, StringView name) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		ecs::World& world	 = host.world();
		if (brains.stategraph == nullptr || slot >= Stategraph::SLOTS || !world.registry.valid(entity))
			return false;
		auto* sg = static_cast<Stategraph*>(brains.stategraph->get(*brains.stategraph, world.registry, entity));
		if (sg == nullptr)
			return false;
		const Graph* graph = brains.graph(sg->slots[slot].graph);
		if (graph == nullptr || graph->find(name) < 0)
			return false;

		// As an event no state ignores would: at the graph's next step, the state's exit, then the new one's entry.
		Event event;
		event.target	= entity;
		event.name		= FORCE_EVENT;
		event.tick		= HostAccess::now(host);
		event.forced	= static_cast<StateId>(hash_text(name));
		event.slot		= static_cast<u8>(slot);
		event.serial	= ++brains.event_serial;
		event.delivered = static_cast<u8>(~(1u << slot)); // for that slot's graph alone
		brains.events.push_back(event);
		return true;
	}

	// --- verbs: e:play(), e:event(), e:strike(), e.rng ----------------------------------------------------

	int entity_play(lua_State* L, Host& host, ecs::Entity entity)
	{
		Host::Brains& brains = HostAccess::brains(host);
		const u64 clip		 = check_name(L, 2);
		if (HostAccess::context(host) == Context::Client)
			return client_play(L, host, entity, clip);
		if (brains.playing == nullptr)
			luaL_error(L, "e:play: the game registered no Playing component (script::register_components)");

		const Exposed* exposed = HostAccess::binding(host).exposed(brains.playing->id);
		const Context context  = HostAccess::context(host);
		if (exposed == nullptr || !may_write(context, *exposed))
			refuse_write(L, context, *exposed, "play");

		auto* playing =
			static_cast<anim::Playing*>(brains.playing->get(*brains.playing, host.world().registry, entity));
		if (playing == nullptr)
			luaL_error(L, "e:play: the entity has no Playing component; give its prefab Playing = {}");

		playing->clip  = clip;
		playing->since = HostAccess::now(host);
		++HostAccess::stats(host).writes;
		return 0;
	}

	int entity_event(lua_State* L, Host& host, ecs::Entity entity)
	{
		// e:event("hurt"), e:event("hurt", striker), or e:event("slash", { by = striker, n = 2 }).
		const char* name = luaL_checkstring(L, 2);
		check_context(L, host, ContextMask::All, "e:event");
		(void)intern_name(host, name);

		ecs::Entity by = ecs::NO_ENTITY;
		f32 value	   = 0.0f;
		if (lua_istable(L, 3))
		{
			lua_getfield(L, 3, "by");
			if (!lua_isnil(L, -1))
				by = check_entity(L, -1);
			lua_pop(L, 1);
			lua_getfield(L, 3, "n");
			if (!lua_isnil(L, -1))
				value = static_cast<f32>(luaL_checknumber(L, -1));
			lua_pop(L, 1);
		}
		else if (!lua_isnoneornil(L, 3))
		{
			by = check_entity(L, 3);
		}
		host.event(entity, name, by, value);
		return 0;
	}

	/**
	 * e:strike{ hits, shape?, ticks? | to?, once? }: the entity's hitbox hits those layers from now, for so many
	 * ticks or until a mark of the state, and never past the state; once, it goes off after its first hit. The
	 * hitbox goes back to what it hit before when the strike ends. From the entity's own graph's handlers.
	 */
	int entity_strike(lua_State* L, Host& host, ecs::Entity entity)
	{
		Host::Brains& brains = HostAccess::brains(host);
		ecs::World& world	 = host.world();
		check_context(L, host, ContextMask::Sim | ContextMask::Server, "e:strike");
		check_sim_target(L, host, entity);
		luaL_checktype(L, 2, LUA_TTABLE);
		if (!brains.running || brains.stepping != entity)
			luaL_error(L, "e:strike is for the entity's own stategraph handlers: it lasts no longer than the state");

		auto* hitbox = world.registry.try_get<physics::Hitbox>(entity);
		if (hitbox == nullptr)
			luaL_error(L, "e:strike: the entity has no Hitbox");
		Strike& strike = world.registry.get_or_emplace<Strike>(entity);

		auto* graphs = static_cast<Stategraph*>(brains.stategraph->get(*brains.stategraph, world.registry, entity));
		const Stategraph::Slot& sg = graphs->slots[brains.stepping_slot];
		Graph* graph			   = brains.graph(sg.graph);
		const State* state		   = state_in(graph, sg);

		u32 hits  = 0;
		bool once = false;
		u32 until = 0xffffffffu;
		lua_pushnil(L);
		while (lua_next(L, 2) != 0)
		{
			const StringView key = lua_isstring(L, -2) ? StringView(lua_tostring(L, -2)) : StringView();
			if (key == "hits")
			{
				const f64 bits = luaL_checknumber(L, -1);
				if (bits < 0.0 || bits > 4294967295.0 || std::floor(bits) != bits)
					luaL_error(L, "e:strike: hits takes layer bits");
				hits = static_cast<u32>(bits);
			}
			else if (key == "shape")
			{
				hitbox->shape = check_shape(L, -1);
			}
			else if (key == "ticks")
			{
				const f64 ticks = luaL_checknumber(L, -1);
				if (ticks < 1.0 || std::floor(ticks) != ticks)
					luaL_error(L, "e:strike: ticks is a whole number from 1");
				until = HostAccess::now(host) + static_cast<u32>(ticks);
			}
			else if (key == "to")
			{
				const char* mark		  = luaL_checkstring(L, -1);
				const Vector<Mark>* marks = graph != nullptr && state != nullptr
												? marks_of(L, host, *graph, *state, entity, brains.stepping_slot, sg)
												: nullptr;
				u32 at					  = 0;
				if (marks == nullptr || !mark_tick(*marks, hash_text(mark), at))
					luaL_error(L, "e:strike: to names a mark the state has not: '%s'", mark);
				until = sg.since + at;
			}
			else if (key == "once")
			{
				once = lua_toboolean(L, -1) != 0;
			}
			else
			{
				luaL_error(L, "e:strike: unknown key '%s'; a strike has hits, shape, ticks, to and once", key.data());
			}
			lua_pop(L, 1);
		}

		if (!strike.active)
			strike.restore = hitbox->hits.bits;
		hitbox->hits.bits = hits;
		strike.until	  = until;
		strike.since	  = sg.since;
		strike.slot		  = static_cast<u8>(brains.stepping_slot);
		strike.once		  = once;
		strike.active	  = true;
		++HostAccess::stats(host).writes;
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

		/**
		 * The entity's next roll this tick: from the tick, the entity and how many it has rolled, so a replay
		 * rolls alike. The entity counts by its network id, which the server and every client share; one
		 * that has none, as a standalone world's have none, by its own number.
		 */
		[[nodiscard]] u32 roll(Host& host, ecs::Entity entity) noexcept
		{
			Host::Brains& brains = HostAccess::brains(host);
			const u32 now		 = HostAccess::now(host);
			if (brains.draws_tick != now)
			{
				brains.draws.clear();
				brains.draws_tick = now;
			}
			const u32 local = static_cast<u32>(entt::to_integral(entity));
			const u32 netid = netid_of(host, entity);
			const u64 id	= netid != 0 ? (u64{1} << 40) | netid : local;
			u32& draws		= brains.draws[local];
			const u64 seed	= (static_cast<u64>(now) << 32) ^ id ^ (static_cast<u64>(draws) << 48);
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

	// --- the declarations: stategraph, g.state, to -------------------------------------------------------

	namespace
	{
		/** The graph a handle names, among those its module is declaring. */
		[[nodiscard]] Graph* staged_graph(lua_State* L, Host& host, const char* name)
		{
			if (!HostAccess::loading(host))
				luaL_error(L, "stategraph %s: states are declared as the module loads, under its stategraph", name);
			for (Graph& graph : HostAccess::brains(host).staging)
				if (graph.name == name && graph.module == static_cast<u32>(HostAccess::loading_module(host)))
					return &graph;
			luaL_error(L, "stategraph %s: no stategraph of that name is being declared by this module", name);
		}

		int state_body(lua_State* L)
		{
			Host& host		  = HostAccess::of(L);
			const char* graph = lua_tostring(L, lua_upvalueindex(1));
			const char* name  = lua_tostring(L, lua_upvalueindex(2));
			luaL_checktype(L, 1, LUA_TTABLE);
			if (HostAccess::def(host).schema != nullptr)
				return 0;
			add_state(L, *staged_graph(L, host, graph), name, 1);
			return 0;
		}

		/** g.state "name": what the state's table goes to. */
		int graph_state(lua_State* L)
		{
			luaL_checkstring(L, 1);
			lua_pushvalue(L, lua_upvalueindex(1)); // the graph's name
			lua_pushvalue(L, 1);
			lua_pushcclosure(L, state_body, "state", 2);
			return 1;
		}

		int stategraph_body(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			const char* name = lua_tostring(L, lua_upvalueindex(1));
			if (!HostAccess::loading(host))
				luaL_error(L, "stategraph is declared at a module's top level, as it loads");
			luaL_checktype(L, 1, LUA_TTABLE);

			const Context context = HostAccess::loading_context(host);
			if (context != Context::Sim && context != Context::Server)
				luaL_error(L, "stategraph %s: a stategraph runs in the simulation, from scripts/sim or scripts/server",
						   name);

			// The handle: its name, and state(name)(def) for the states declared under it.
			const auto push_handle = [&]
			{
				lua_createtable(L, 0, 2);
				lua_pushstring(L, name);
				lua_setfield(L, -2, "name");
				lua_pushstring(L, name);
				lua_pushcclosure(L, graph_state, "state", 1);
				lua_setfield(L, -2, "state");
			};

			if (Schema* schema = HostAccess::def(host).schema; schema != nullptr)
			{
				for (const String& known : schema->stategraphs)
					if (known == name)
						luaL_error(L, "stategraph %s is declared twice", name);
				schema->stategraphs.push_back(String(name, &heap()));
				push_handle();
				return 1;
			}

			Graph graph;
			graph.name	   = String(name, &heap());
			graph.id.value = static_cast<u32>(hash_text(name));
			graph.module   = static_cast<u32>(HostAccess::loading_module(host));
			graph.context  = context;
			(void)intern_name(host, name);
			read_graph(L, host, 1, graph);
			HostAccess::brains(host).staging.push_back(std::move(graph));
			push_handle();
			return 1;
		}

		int lua_stategraph(lua_State* L)
		{
			luaL_checkstring(L, 1);
			lua_pushvalue(L, 1);
			lua_pushcclosure(L, stategraph_body, "stategraph", 1);
			return 1;
		}

		/** to "state": a handler that moves on. */
		int lua_to(lua_State* L)
		{
			luaL_checkstring(L, 1);
			lua_createtable(L, 0, 1);
			lua_pushvalue(L, 1);
			lua_setfield(L, -2, "__to");
			return 1;
		}
	}

	void install_stategraphs(lua_State* L, Host& host)
	{
		(void)host;
		lua_setlightuserdataname(L, TAG_RNG, "Rng");
		lua_pushcfunction(L, lua_stategraph, "stategraph");
		lua_setglobal(L, "stategraph");
		lua_pushcfunction(L, lua_to, "to");
		lua_setglobal(L, "to");
	}
}
