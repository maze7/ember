#include "internal.h"

#include <ember/audio/components.h>
#include <ember/core/hash.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>

#include <cmath>
#include <cstring>

/**
 * Shows: what clients show of what the simulation does. A state's, a graph's or a prefab's `show` table
 * holds handlers for its entry and exit, for every frame, and for its marks and the cues raised on it,
 * by name. The host runs them on a client at the moment each is drawn: a state as the replicated graph
 * enters it, a mark as its tick is drawn, a cue as the tick it was raised on is drawn. A handler gets
 * the entity as a Shown: this client's verbs on it, flash, squash, sound and the rest, and none of the
 * game's. Loaded on every machine, run on clients alone.
 *
 * Also here: the `show "prefab" { ... }` declaration, for prefabs without a graph, and the client verbs.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		/** Seconds after its moment a one-shot is still shown; later, this client turned up after it. */
		constexpr f64 LATE_SECONDS = 0.5;

		/** The moment a tick is drawn, in ticks: as the tick before it ends, since a tick's work is drawn as the moment
		 * runs up to it. */
		[[nodiscard]] f64 drawn_at(u32 tick) noexcept { return static_cast<f64>(tick) - 1.0; }

		/** The text of a cue's name, found by the low half of its hash among the names scripts have spelt. */
		[[nodiscard]] StringView name_for(const Host::Brains& brains, u32 low) noexcept
		{
			for (const auto& [hash, text] : brains.names)
				if (static_cast<u32>(hash) == low)
					return text;
			return {};
		}

		/** Where the handlers of a show come from, and what to switch off when one fails. */
		struct Owner
		{
			u32 module;
			bool* disabled;
			StringView what;
		};

		/**
		 * Calls a show handler with the entity and, for a cue, its table: { name, tick, by, n, late }. The
		 * client's rights, a fresh budget, and the handler's moment for the verbs. False on an error, which
		 * reports it and switches the show off until its module reloads.
		 */
		bool call(Host& host, const Owner& owner, int ref, ecs::Entity entity, f64 moment, const Cue* cue, u64 cue_name,
				  f64 late_seconds, ecs::Entity by)
		{
			Host::Brains& brains = HostAccess::brains(host);
			lua_State* thread	 = HostAccess::thread_of(host, owner.module);
			lua_settop(thread, 0);
			lua_getref(thread, ref);
			push_entity(thread, entity);

			int args = 1;
			if (cue != nullptr)
			{
				lua_createtable(thread, 0, 5);
				const StringView text = cue_name != 0 ? name_text(thread, cue_name) : name_for(brains, cue->name);
				if (text.empty())
					lua_pushfstring(thread, "%08x", static_cast<unsigned>(cue->name));
				else
					lua_pushlstring(thread, text.data(), text.size());
				lua_setfield(thread, -2, "name");
				lua_pushnumber(thread, static_cast<f64>(cue->tick));
				lua_setfield(thread, -2, "tick");
				if (by != ecs::NO_ENTITY)
				{
					push_entity(thread, by);
					lua_setfield(thread, -2, "by");
				}
				lua_pushnumber(thread, static_cast<f64>(cue->value));
				lua_setfield(thread, -2, "n");
				lua_pushnumber(thread, late_seconds);
				lua_setfield(thread, -2, "late");
				args = 2;
			}

			brains.moment = moment;
			HostAccess::begin_call(host, Context::Client);
			const int status = lua_pcall(thread, args, 0, 0);
			HostAccess::end_call(host);
			++HostAccess::stats(host).calls;

			if (status != LUA_OK)
			{
				String message(&heap());
				message += owner.what;
				message += ": ";
				message += lua_tostring(thread, -1) != nullptr ? lua_tostring(thread, -1) : "error";
				HostAccess::report_lua(host, HostAccess::path_of(host, owner.module), message.c_str());
				lua_settop(thread, 0);
				*owner.disabled = true;
				++HostAccess::stats(host).errors;
				return false;
			}
			lua_settop(thread, 0);
			return true;
		}

		/** The show's handler for a cue's name, in the state's show, then the graph's, then the prefab's. */
		struct Answer
		{
			int fn = -1;
			Owner owner{0, nullptr, {}};
		};

		[[nodiscard]] Answer answer(Host::Brains& brains, Graph* graph, const State* state, PrefabShow* prefab,
									const Shown& shown, u64 name, u32 low)
		{
			// By the whole hash when the cue was raised here, by its low half when it came over the wire.
			const auto fn_of = [&](const Show& show) { return name != 0 ? show.rule(name) : show.rule_low(low); };
			if (graph != nullptr && !graph->show_disabled)
			{
				if (state != nullptr)
					if (const int fn = fn_of(state->show); fn >= 0)
						return {fn, {graph->module, &graph->show_disabled, "a show handler"}};
				if (const int fn = fn_of(graph->show); fn >= 0)
					return {fn, {graph->module, &graph->show_disabled, "a show handler"}};
			}
			if (prefab != nullptr && !prefab->disabled)
				if (const int fn = fn_of(prefab->show); fn >= 0)
					return {fn, {prefab->module, &prefab->disabled, "a prefab show handler"}};

			// Then the modifiers the entity is shown with: a burn's show answers the "burn" its tick raises.
			for (u32 i = 0; i < shown.modifier_count; ++i)
				if (ModifierDecl* decl = brains.modifier(shown.modifiers[i]); decl != nullptr && !decl->show_disabled)
					if (const int fn = fn_of(decl->show); fn >= 0)
						return {fn, {decl->module, &decl->show_disabled, "a modifier show handler"}};
			return {};
		}

		/** One cue, raised at `tick` with this name, shown once at its moment, or skipped as seen too late. */
		void show_cue(Host& host, ecs::Entity entity, Shown& shown, Graph* graph, const State* state,
					  PrefabShow* prefab, const Cue& cue, u64 name, ecs::Entity by, f64 moment, f64 seconds_per_tick)
		{
			if (shown.has_seen(cue.tick, cue.name) || moment < drawn_at(cue.tick))
				return;
			shown.see(cue.tick, cue.name);

			const f64 late = (moment - drawn_at(cue.tick)) * seconds_per_tick;
			if (late > LATE_SECONDS)
				return;

			const Answer found = answer(HostAccess::brains(host), graph, state, prefab, shown, name, cue.name);
			if (found.fn >= 0)
				(void)call(host, found.owner, found.fn, entity, drawn_at(cue.tick), &cue, name, late, by);
		}
	}

	void read_show(lua_State* L, int index, const char* what, Show& out)
	{
		if (index < 0)
			index = lua_gettop(L) + 1 + index;
		if (!lua_istable(L, index))
			luaL_error(L, "%s: show is { enter = fn, exit = fn, update = fn, <mark or cue> = fn, ... }", what);

		lua_pushnil(L);
		while (lua_next(L, index) != 0)
		{
			if (!lua_isstring(L, -2))
				luaL_error(L, "%s: a show's keys are names", what);
			if (!lua_isfunction(L, -1))
				luaL_error(L, "%s: show.%s takes a function, not %s", what, lua_tostring(L, -2), luaL_typename(L, -1));

			const StringView key = lua_tostring(L, -2);
			if (key == "enter")
				out.enter = lua_ref(L, -1);
			else if (key == "exit")
				out.exit = lua_ref(L, -1);
			else if (key == "update")
				out.update = lua_ref(L, -1);
			else
				out.on.push_back({.name = intern_name(HostAccess::of(L), key), .fn = lua_ref(L, -1)});
			lua_pop(L, 1);
		}
	}

	void release_show(lua_State* L, Show& show) noexcept
	{
		for (const int ref : {show.enter, show.exit, show.update})
			if (ref >= 0)
				lua_unref(L, ref);
		for (const ShowRule& rule : show.on)
			if (rule.fn >= 0)
				lua_unref(L, rule.fn);
		show.enter = show.exit = show.update = -1;
		show.on.clear();
	}

	void gather_shown_names(Host& host) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		brains.shown_names.clear();
		const auto add = [&](const Show& show)
		{
			for (const ShowRule& rule : show.on)
				if (!brains.answers(rule.name))
					brains.shown_names.push_back(rule.name);
		};
		for (const Graph& graph : brains.graphs)
		{
			add(graph.show);
			for (const State& state : graph.states)
				add(state.show);
		}
		for (const PrefabShow& prefab : brains.prefab_shows)
			add(prefab.show);
		for (const ModifierDecl& modifier : brains.modifiers)
			add(modifier.show);
	}

	void run_shows(Host& host) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		ecs::World& world	 = host.world();
		if (!brains.presenting || world.role() == ecs::Role::Server)
			return;

		EMBER_PROFILE_SCOPE_C("script shows", PROFILE_COLOR_GAMEPLAY);
		const f64 seconds_per_tick = HostAccess::binding(host).ticks_per_second() > 0.0
										 ? 1.0 / HostAccess::binding(host).ticks_per_second()
										 : 1.0 / 60.0;

		for (const auto [entity, ref] : world.registry.view<const ecs::PrefabRef>().each())
		{
			// What there is to show of it: its graphs' states, its prefab's show, its cues.
			auto* sg =
				brains.stategraph != nullptr
					? static_cast<Stategraph*>(brains.stategraph->get(*brains.stategraph, world.registry, entity))
					: nullptr;
			PrefabShow* prefab = nullptr;
			for (PrefabShow& candidate : brains.prefab_shows)
				if (candidate.prefab == ref.id)
					prefab = &candidate;
			auto* cues				   = brains.cues != nullptr
											 ? static_cast<Cues*>(brains.cues->get(*brains.cues, world.registry, entity))
											 : nullptr;
			const Modifiers* modifiers = world.registry.try_get<Modifiers>(entity);
			if (sg == nullptr && prefab == nullptr && cues == nullptr &&
				(modifiers == nullptr || modifiers->count == 0))
			{
				// Nothing to show, unless a modifier it was shown with has gone: its exit is still owed.
				const Shown* known = world.registry.try_get<Shown>(entity);
				if (known == nullptr || known->modifier_count == 0)
					continue;
			}

			Shown& shown	 = world.registry.get_or_emplace<Shown>(entity);
			const bool own	 = world.registry.all_of<net::Owned>(entity);
			const f64 moment = own ? brains.predicted : brains.interpolated;

			// Each slot's graph: the state as drawn, its entry and exit, its marks as their ticks are drawn,
			// and every frame in it.
			Graph* graphs[Stategraph::SLOTS]	   = {};
			const State* states[Stategraph::SLOTS] = {};
			for (u32 slot = 0; sg != nullptr && slot < Stategraph::SLOTS; ++slot)
			{
				const Stategraph::Slot& live = sg->slots[slot];
				Shown::Slot& seen			 = shown.slots[slot];
				Graph* graph				 = brains.graph(live.graph);
				if (graph == nullptr)
					continue;
				graphs[slot] = graph;
				const Owner owner{graph->module, &graph->show_disabled, "a show handler"};

				if ((live.state != seen.state || live.since != seen.since) && live.state != NO_STATE &&
					moment >= drawn_at(live.since))
				{
					if (const i16 old = graph->index_of(seen.state); old >= 0 && !graph->show_disabled)
					{
						const State& was = graph->states[static_cast<size_t>(old)];
						if (was.show.exit >= 0)
							(void)call(host, owner, was.show.exit, entity, drawn_at(live.since), nullptr, 0, 0.0,
									   ecs::NO_ENTITY);
					}
					seen.state = live.state;
					seen.since = live.since;
					seen.marks = 0;
					seen.late  = (moment - drawn_at(live.since)) * seconds_per_tick > LATE_SECONDS;

					if (const i16 index = graph->index_of(live.state); index >= 0 && !graph->show_disabled)
					{
						const State& now = graph->states[static_cast<size_t>(index)];
						if (now.show.enter >= 0 && !seen.late)
							(void)call(host, owner, now.show.enter, entity, drawn_at(live.since), nullptr, 0, 0.0,
									   ecs::NO_ENTITY);
					}
				}

				const i16 index = graph->index_of(seen.state);
				if (index < 0)
					continue;
				const State& state = graph->states[static_cast<size_t>(index)];
				states[slot]	   = &state;
				if (graph->show_disabled)
					continue;

				// Marks, in tick order, each as its tick is drawn; a late state's that are long past are skipped.
				const f64 elapsed = moment - drawn_at(seen.since);
				while (seen.marks < state.marks.size() && static_cast<f64>(state.marks[seen.marks].at) <= elapsed)
				{
					const Mark& mark = state.marks[seen.marks++];
					const f64 at	 = drawn_at(seen.since) + static_cast<f64>(mark.at);
					if ((moment - at) * seconds_per_tick > LATE_SECONDS)
						continue;
					const int fn =
						state.show.rule(mark.name) >= 0 ? state.show.rule(mark.name) : graph->show.rule(mark.name);
					if (fn >= 0 && !call(host, owner, fn, entity, at, nullptr, 0, 0.0, ecs::NO_ENTITY))
						break;
				}
				if (graph->show_disabled)
					continue;

				const int update = state.show.update >= 0 ? state.show.update : graph->show.update;
				if (update >= 0)
					(void)call(host, owner, update, entity, moment, nullptr, 0, 0.0, ecs::NO_ENTITY);
			}

			// The prefab's show: once at first sight, then every frame.
			if (prefab != nullptr && !prefab->disabled)
			{
				const Owner owner{prefab->module, &prefab->disabled, "a prefab show handler"};
				if (shown.fresh && prefab->show.enter >= 0)
					(void)call(host, owner, prefab->show.enter, entity, moment, nullptr, 0, 0.0, ecs::NO_ENTITY);
				if (prefab->show.update >= 0 && !prefab->disabled)
					(void)call(host, owner, prefab->show.update, entity, moment, nullptr, 0, 0.0, ecs::NO_ENTITY);
			}
			shown.fresh = false;

			// The modifiers' shows: entered as one appears on it, every frame while it stays, and left as it goes,
			// after the cues, so the last of what its tick raised is answered first.
			const auto modifier_owner = [](ModifierDecl& decl) -> Owner
			{ return {decl.module, &decl.show_disabled, "a modifier show handler"}; };
			for (u32 k = 0; modifiers != nullptr && k < modifiers->count; ++k)
			{
				const u32 id	   = modifiers->list[k].name;
				ModifierDecl* decl = brains.modifier(id);
				bool entered	   = false;
				for (u32 i = 0; i < shown.modifier_count; ++i)
					entered = entered || shown.modifiers[i] == id;
				if (!entered && shown.modifier_count < Modifiers::CAPACITY)
				{
					shown.modifiers[shown.modifier_count++] = id;
					if (decl != nullptr && !decl->show_disabled && decl->show.enter >= 0)
						(void)call(host, modifier_owner(*decl), decl->show.enter, entity, moment, nullptr, 0, 0.0,
								   ecs::NO_ENTITY);
				}
				if (decl != nullptr && !decl->show_disabled && decl->show.update >= 0)
					(void)call(host, modifier_owner(*decl), decl->show.update, entity, moment, nullptr, 0, 0.0,
							   ecs::NO_ENTITY);
			}

			// The cues: the server's, replicated, oldest first; and this client's own, raised on what it
			// predicts, which the server's copy of is then one it has seen.
			const Graph* shown_graph = graphs[0] != nullptr ? graphs[0] : graphs[1];
			const State* shown_state = graphs[0] != nullptr ? states[0] : states[1];
			if (cues != nullptr)
			{
				for (u32 i = 0; i < cues->count; ++i)
				{
					const Cue& cue		 = cues->ring[(cues->next + Cues::CAPACITY - cues->count + i) % Cues::CAPACITY];
					const ecs::Entity by = cue.by != 0 ? entity_of_netid(host, cue.by) : ecs::NO_ENTITY;
					show_cue(host, entity, shown, const_cast<Graph*>(shown_graph), shown_state, prefab, cue, 0, by,
							 moment, seconds_per_tick);
				}
			}
			if (!brains.own_cues.empty())
			{
				for (const Event& event : brains.own_cues)
				{
					if (event.target != entity)
						continue;
					const Cue cue{
						.name = static_cast<u32>(event.name), .tick = event.tick, .by = 0, .value = event.value};
					show_cue(host, entity, shown, const_cast<Graph*>(shown_graph), shown_state, prefab, cue, event.name,
							 event.source, brains.predicted, seconds_per_tick);
				}
			}

			for (u32 i = shown.modifier_count; i-- > 0;)
			{
				const u32 id = shown.modifiers[i];
				bool still	 = false;
				for (u32 k = 0; modifiers != nullptr && k < modifiers->count; ++k)
					still = still || modifiers->list[k].name == id;
				if (still)
					continue;
				shown.modifiers[i] = shown.modifiers[--shown.modifier_count];
				if (ModifierDecl* decl = brains.modifier(id);
					decl != nullptr && !decl->show_disabled && decl->show.exit >= 0)
					(void)call(host, modifier_owner(*decl), decl->show.exit, entity, moment, nullptr, 0, 0.0,
							   ecs::NO_ENTITY);
			}
		}

		brains.own_cues.clear();
	}

	// --- the client's verbs ---------------------------------------------------------------------------

	namespace
	{
		/** The handler's moment on the animation clock, in seconds: what a clip or a sound starts at. */
		[[nodiscard]] f64 moment_seconds(Host& host, ecs::Entity entity)
		{
			const Host::Brains& brains = HostAccess::brains(host);
			const f64 per_second	   = HostAccess::binding(host).ticks_per_second();
			const bool own			   = host.world().registry.all_of<net::Owned>(entity);
			const f64 shift			   = own ? 0.0 : brains.predicted - brains.interpolated;
			return (brains.moment + shift) / (per_second > 0.0 ? per_second : 60.0);
		}

		[[nodiscard]] anim::Animator& animator_of(lua_State* L, Host& host, ecs::Entity entity, const char* verb)
		{
			anim::Animator* animator = host.world().registry.try_get<anim::Animator>(entity);
			if (animator == nullptr)
				luaL_error(L, "%s: Entity(%u) has no Animator to show it with", verb,
						   static_cast<unsigned>(entt::to_integral(entity)));
			return *animator;
		}
	}

	int client_verb(lua_State* L, Host& host, ecs::Entity entity, Verb verb)
	{
		check_context(L, host, ContextMask::Client, "a client verb");
		if (!HostAccess::brains(host).presenting && verb != Verb::Mine)
			luaL_error(L, "the client's verbs are for show handlers and Present systems");

		switch (verb)
		{
			case Verb::Flash:
			{
				// White for this long, from the handler's moment: the rig's "flash" overlay.
				const f32 seconds = static_cast<f32>(luaL_checknumber(L, 2));
				animator_of(L, host, entity, "flash").overlay("flash", moment_seconds(host, entity), seconds);
				return 0;
			}
			case Verb::Overlay:
			{
				const u64 clip			 = check_name(L, 2);
				const f32 seconds		 = static_cast<f32>(luaL_optnumber(L, 3, 0.0));
				anim::Animator& animator = animator_of(L, host, entity, "overlay");
				const StringView text	 = name_text(L, clip);
				if (text.empty())
					luaL_error(L, "overlay: a clip by name");
				animator.overlay(text, moment_seconds(host, entity), seconds);
				return 0;
			}
			case Verb::Squash:
			{
				const f32 x		  = static_cast<f32>(luaL_checknumber(L, 2));
				const f32 y		  = static_cast<f32>(luaL_checknumber(L, 3));
				const f32 seconds = static_cast<f32>(luaL_checknumber(L, 4));
				animator_of(L, host, entity, "squash").squash({x, y}, moment_seconds(host, entity), seconds);
				return 0;
			}
			case Verb::Lean:
				animator_of(L, host, entity, "lean").lean = static_cast<f32>(luaL_checknumber(L, 2));
				return 0;
			case Verb::Hold:
			{
				const f64 seconds = luaL_checknumber(L, 2);
				const f64 from	  = moment_seconds(host, entity);
				animator_of(L, host, entity, "hold").hold(from, from + seconds);
				return 0;
			}
			case Verb::Sound:
			{
				// Heard once at the handler's moment, however often the frame says it. The entity gets an
				// emitter if it has none; the audio systems place it where the entity stands.
				const char* path = luaL_checkstring(L, 2);
				auto& emitter	 = host.world().registry.get_or_emplace<audio::Emitter>(entity);
				if (!lua_isnoneornil(L, 3))
				{
					luaL_checktype(L, 3, LUA_TTABLE);
					lua_pushnil(L);
					while (lua_next(L, 3) != 0)
					{
						if (!lua_isstring(L, -2) || !lua_isnumber(L, -1))
							luaL_error(L, "sound: params are { name = number }");
						emitter.set(audio::Param::from(lua_tostring(L, -2)), static_cast<f32>(lua_tonumber(L, -1)));
						lua_pop(L, 1);
					}
				}
				emitter.play(audio::Event::from(path), moment_seconds(host, entity));
				return 0;
			}
			case Verb::Mine:
				lua_pushboolean(L, host.world().registry.all_of<net::Owned>(entity));
				return 1;
			default:
				luaL_error(L, "not a client verb");
		}
	}

	int client_play(lua_State* L, Host& host, ecs::Entity entity, u64 clip)
	{
		if (!HostAccess::brains(host).presenting)
			luaL_error(L, "e:play on a client is for show handlers and Present systems");
		animator_of(L, host, entity, "play")
			.play(static_cast<anim::Name>(clip), {.since = moment_seconds(host, entity)});
		return 0;
	}

	// --- show "prefab" { ... } ------------------------------------------------------------------------

	namespace
	{
		int show_body(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			const char* name = lua_tostring(L, lua_upvalueindex(1));
			if (!HostAccess::loading(host))
				luaL_error(L, "show is declared at a module's top level, as it loads");
			luaL_checktype(L, 1, LUA_TTABLE);

			// The schema pass wants declarations alone; a show is let go.
			if (HostAccess::def(host).schema != nullptr)
				return 0;

			const ecs::Prefab* prefab = host.world().prefabs().find(name);
			if (prefab == nullptr)
				luaL_error(L, "show %s: no prefab of that name", name);

			PrefabShow show;
			show.prefab = prefab->id;
			show.module = static_cast<u32>(HostAccess::loading_module(host));
			String what("show ", &heap());
			what += name;
			read_show(L, 1, what.c_str(), show.show);
			HostAccess::brains(host).prefab_shows_staging.push_back(std::move(show));
			return 0;
		}

		int lua_show(lua_State* L)
		{
			luaL_checkstring(L, 1);
			lua_pushvalue(L, 1);
			lua_pushcclosure(L, show_body, "show", 1);
			return 1;
		}
	}

	void install_shows(lua_State* L, Host& host)
	{
		(void)host;
		lua_pushcfunction(L, lua_show, "show");
		lua_setglobal(L, "show");
	}
}
