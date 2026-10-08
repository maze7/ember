#include "internal.h"

#include <ember/core/hash.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>

#include <algorithm>
#include <cstring>

/**
 * Stories: a coroutine an entity runs, on the server or on a client, that reads top to bottom and waits
 * where it says. `story "x" { prefab, run, on, loop }` declares one; an entity gets it from its prefab,
 * from a `Story = "x"` component in a prefab, or from e:start("x"). A story's thread lives in the host,
 * never in a component: a reload of its module, a lost entity, or e:stop() drops the thread, and a
 * story that should go on from there starts `run` again from the top, so what must last between the
 * two lives in components. The server's run at a stage's script point, once a tick; a client's at
 * Present, each frame, waiting in ticks of the moment it draws.
 *
 *   wait(ticks)             sleeps that many ticks; wait(0) is the next point
 *   wait_until(fn, every?)  calls fn each point (or every so many ticks) until it answers something,
 *                           and gives that answer
 *   wait_for("event")       sleeps until that event is raised on the entity, and gives it
 *
 * `on = { event = fn }` are reactions: plain functions, run as the event arrives; one that returns
 * "restart" starts `run` over.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		constexpr StringView STORY_KEYS[] = {"prefab", "stage", "loop", "run", "on"};

		[[nodiscard]] u64 thread_key(ecs::Entity entity, u32 story) noexcept
		{
			return (static_cast<u64>(entt::to_integral(entity)) << 32) | story;
		}

		/** The story a thread runs; null once its module has gone. */
		[[nodiscard]] StoryDecl* decl_of(Host::Brains& brains, u32 story) noexcept
		{
			for (StoryDecl& candidate : brains.stories)
				if (candidate.id == story)
					return &candidate;
			return nullptr;
		}

		void drop(lua_State* L, StoryThread& thread) noexcept
		{
			if (thread.co_ref >= 0)
				lua_unref(L, thread.co_ref);
			if (thread.until_fn >= 0)
				lua_unref(L, thread.until_fn);
			thread.co		= nullptr;
			thread.co_ref	= -1;
			thread.until_fn = -1;
			thread.wait		= StoryThread::Wait::None;
		}

		/** A fresh coroutine over the story's run, on its module's globals; nothing run yet. */
		bool start_thread(Host& host, StoryThread& thread, const StoryDecl& decl)
		{
			// A story of reactions alone has nothing to run: it is at rest, and its `on` answers events.
			if (decl.run < 0)
			{
				thread.wait	  = StoryThread::Wait::None;
				thread.failed = false;
				thread.done	  = true;
				return false;
			}
			lua_State* L	  = host.state();
			lua_State* module = HostAccess::thread_of(host, decl.module);
			lua_State* co	  = lua_newthread(module);
			thread.co		  = co;
			thread.co_ref	  = lua_ref(module, -1);
			lua_pop(module, 1);
			(void)L;
			lua_getref(co, decl.run);
			push_entity(co, thread.entity);
			thread.wait	  = StoryThread::Wait::Start; // the first resume passes the entity
			thread.wake	  = 0;
			thread.failed = false;
			thread.done	  = false;
			thread.line	  = 0;
			return true;
		}

		void report_failure(Host& host, StoryThread& thread, const StoryDecl& decl, const char* message)
		{
			String text("story ", &heap());
			text += decl.name;
			text += ": ";
			text += message != nullptr ? message : "error";
			HostAccess::report_lua(host, HostAccess::path_of(host, decl.module), text.c_str());
			thread.failed = true;
			++HostAccess::stats(host).errors;
		}

		/** Where a suspended story stands: the line of the function that called wait, for the inspector. */
		void note_line(StoryThread& thread) noexcept
		{
			lua_Debug ar;
			thread.line = 0;
			for (int level = 0; level < 4; ++level)
			{
				if (!lua_getinfo(thread.co, level, "l", &ar))
					break;
				if (ar.currentline > 0)
				{
					thread.line = static_cast<u32>(ar.currentline);
					break;
				}
			}
		}

		/**
		 * Resumes a thread with `args` values on its stack. What it does next is in the thread's wait once
		 * it yields; finished, it is done, or starts over when its story loops.
		 */
		void resume(Host& host, StoryThread& thread, StoryDecl& decl, int args, u32 now)
		{
			Host::Brains& brains = HostAccess::brains(host);
			brains.resuming		 = &thread;
			brains.resuming_now	 = now;
			HostAccess::begin_call(host, decl.context);
			const int status = lua_resume(thread.co, nullptr, args);
			HostAccess::end_call(host);
			brains.resuming = nullptr;
			++HostAccess::stats(host).calls;

			if (status == LUA_YIELD)
			{
				note_line(thread);
				lua_settop(thread.co, 0);
				return;
			}
			if (status == LUA_OK)
			{
				// Finished: again from the top when the story loops, else at rest until it is started again.
				lua_settop(thread.co, 0);
				if (decl.loop)
				{
					drop(host.state(), thread);
					(void)start_thread(host, thread, decl);
				}
				else
				{
					drop(host.state(), thread);
					thread.done = true;
				}
				return;
			}

			report_failure(host, thread, decl, lua_tostring(thread.co, -1));
			drop(host.state(), thread);
		}

		/** The next step of a waiting thread this point: resumes it when its wait is over. */
		void step(Host& host, StoryThread& thread, StoryDecl& decl, u32 now)
		{
			if (thread.failed || thread.done)
				return;
			if (thread.co == nullptr && !start_thread(host, thread, decl))
				return;

			switch (thread.wait)
			{
				case StoryThread::Wait::Start:
					thread.wait = StoryThread::Wait::None;
					resume(host, thread, decl, 1, now);
					return;
				case StoryThread::Wait::Ticks:
					if (now >= thread.wake)
					{
						thread.wait = StoryThread::Wait::None;
						resume(host, thread, decl, 0, now);
					}
					return;
				case StoryThread::Wait::Until:
				{
					if (now < thread.wake)
						return;
					thread.wake = now + thread.every;

					// The check, on its module's thread: anything but nil or false ends the wait, and is the answer.
					lua_State* module = HostAccess::thread_of(host, decl.module);
					lua_settop(module, 0);
					lua_getref(module, thread.until_fn);
					HostAccess::begin_call(host, decl.context);
					const int status = lua_pcall(module, 0, 1, 0);
					HostAccess::end_call(host);
					if (status != LUA_OK)
					{
						report_failure(host, thread, decl, lua_tostring(module, -1));
						lua_settop(module, 0);
						drop(host.state(), thread);
						return;
					}
					if (lua_toboolean(module, -1))
					{
						lua_xmove(module, thread.co, 1);
						lua_unref(host.state(), thread.until_fn);
						thread.until_fn = -1;
						thread.wait		= StoryThread::Wait::None;
						resume(host, thread, decl, 1, now);
					}
					lua_settop(module, 0);
					return;
				}
				case StoryThread::Wait::Event:
				case StoryThread::Wait::None:
					return;
			}
		}

		/** The thread of an entity's story, made if it has none. */
		[[nodiscard]] StoryThread& thread_for(Host::Brains& brains, ecs::Entity entity, u32 story)
		{
			const u64 key = thread_key(entity, story);
			const auto it = brains.threads.find(key);
			if (it != brains.threads.end())
				return brains.threads[key];
			StoryThread& made = brains.threads[key];
			made.entity		  = entity;
			made.story		  = story;
			return made;
		}

		/** Whether an entity has a story: by its prefab, or in a Story slot. */
		[[nodiscard]] bool attached(Host& host, ecs::Entity entity, const StoryDecl& decl)
		{
			ecs::World& world = host.world();
			if (decl.prefab != ecs::NO_PREFAB)
			{
				const ecs::PrefabRef* ref = world.registry.try_get<ecs::PrefabRef>(entity);
				if (ref != nullptr && ref->id == decl.prefab)
					return true;
			}
			if (const Story* story = world.registry.try_get<Story>(entity))
				for (const u32 slot : story->slots)
					if (slot == decl.id)
						return true;
			return false;
		}

		[[nodiscard]] const char* wait_name(StoryThread::Wait wait) noexcept
		{
			switch (wait)
			{
				case StoryThread::Wait::Ticks:
					return "wait";
				case StoryThread::Wait::Until:
					return "wait_until";
				case StoryThread::Wait::Event:
					return "wait_for";
				default:
					return "";
			}
		}
	}

	void run_stories(Host& host, u8 stage, bool present) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		ecs::World& world	 = host.world();
		if (brains.stories.empty())
			return;

		EMBER_PROFILE_SCOPE_C("script stories", PROFILE_COLOR_GAMEPLAY);
		const u32 now = present ? static_cast<u32>(std::max(brains.predicted, 0.0)) : HostAccess::now(host);

		// What e:stop(), e:start() and reloads asked for since the last pass, before threads are made or resumed.
		sweep_threads(host);

		// Every entity a story is attached to has a thread for it; those whose entity or attachment went are
		// dropped. The stories this pass runs, then one look at each entity: by prefab, then by component.
		const auto runs_now = [&](const StoryDecl& decl)
		{
			return !decl.disabled && runs_in(decl.context, world.role()) &&
				   present == (decl.context == Context::Client) && (present || decl.stage == stage);
		};
		Vector<const StoryDecl*> by_prefab(&heap());
		bool any = false;
		for (const StoryDecl& decl : brains.stories)
		{
			if (!runs_now(decl))
				continue;
			any = true;
			if (decl.prefab != ecs::NO_PREFAB)
				by_prefab.push_back(&decl);
		}
		if (!by_prefab.empty())
		{
			for (const auto [entity, ref] : world.registry.view<const ecs::PrefabRef>().each())
				for (const StoryDecl* decl : by_prefab)
					if (decl->prefab == ref.id && (present || world.registry.all_of<ecs::Simulated>(entity)))
						(void)thread_for(brains, entity, decl->id);
		}
		if (any)
		{
			for (const auto [entity, story] : world.registry.view<const Story>().each())
				for (const u32 slot : story.slots)
					if (const StoryDecl* decl = slot != 0 ? decl_of(brains, slot) : nullptr;
						decl != nullptr && runs_now(*decl) &&
						(present || world.registry.all_of<ecs::Simulated>(entity)))
						(void)thread_for(brains, entity, decl->id);
		}

		// The newest event every thread will have seen once this pass has handed them out: what is raised during
		// it waits for the next point, and comes after.
		u32 newest = 0;
		for (const Event& event : brains.events)
			newest = std::max(newest, event.serial);

		Vector<u64> gone(&heap());
		for (auto& [key, thread] : brains.threads)
		{
			// Another point's story is looked after at that point; one whose entity or attachment went is dropped.
			StoryDecl* decl = decl_of(brains, thread.story);
			if (decl != nullptr && !runs_now(*decl))
				continue;
			if (decl == nullptr || !world.registry.valid(thread.entity) || !attached(host, thread.entity, *decl))
			{
				drop(host.state(), thread);
				gone.push_back(key);
				continue;
			}
			if (thread.sweep != StoryThread::Sweep::None)
				continue;

			// Its events first, each once: reactions, and the one a wait_for sleeps on.
			if (!brains.events.empty())
			{
				for (size_t i = 0;
					 i < brains.events.size() && !thread.failed && thread.sweep == StoryThread::Sweep::None; ++i)
				{
					Event& event = brains.events[i];
					if (event.target != thread.entity || event.serial <= thread.seen)
						continue;

					if (const int fn = decl->reaction(event.name); fn >= 0)
					{
						lua_State* module = HostAccess::thread_of(host, decl->module);
						lua_settop(module, 0);
						lua_getref(module, fn);
						push_entity(module, thread.entity);
						push_event_table(module, host, event);
						HostAccess::begin_call(host, decl->context);
						const int status = lua_pcall(module, 2, 1, 0);
						HostAccess::end_call(host);
						++HostAccess::stats(host).calls;
						if (status != LUA_OK)
						{
							report_failure(host, thread, *decl, lua_tostring(module, -1));
							drop(host.state(), thread);
						}
						else if (lua_isstring(module, -1) && StringView(lua_tostring(module, -1)) == "restart")
						{
							drop(host.state(), thread);
							thread.done = false;
							(void)start_thread(host, thread, *decl);
						}
						lua_settop(module, 0);
					}

					if (!thread.failed && thread.co != nullptr && thread.wait == StoryThread::Wait::Event &&
						event.name == thread.event_name && thread.sweep == StoryThread::Sweep::None)
					{
						push_event_table(thread.co, host, event);
						thread.wait = StoryThread::Wait::None;
						resume(host, thread, *decl, 1, now);
					}
				}
			}

			thread.seen = std::max(thread.seen, newest);
			if (thread.sweep == StoryThread::Sweep::None)
				step(host, thread, *decl, now);
		}
		for (const u64 key : gone)
			brains.threads.erase(key);
		sweep_threads(host);
	}

	// --- wait, wait_until, wait_for ------------------------------------------------------------------------

	namespace
	{
		[[nodiscard]] StoryThread& check_story(lua_State* L, const char* what)
		{
			Host::Brains& brains = HostAccess::brains(HostAccess::of(L));
			if (brains.resuming == nullptr || brains.resuming->co != L)
				luaL_error(L, "%s is for a story's run: a system or a handler cannot wait", what);
			return *brains.resuming;
		}

		int lua_wait(lua_State* L)
		{
			StoryThread& thread = check_story(L, "wait");
			const f64 ticks		= luaL_checknumber(L, 1);
			if (ticks < 0.0 || ticks > 4294967295.0 || ticks != static_cast<f64>(static_cast<u32>(ticks)))
				luaL_error(L, "wait takes a whole number of ticks");
			thread.wait = StoryThread::Wait::Ticks;
			thread.wake =
				HostAccess::brains(HostAccess::of(L)).resuming_now + std::max<u32>(static_cast<u32>(ticks), 1);
			return lua_yield(L, 0);
		}

		int lua_wait_until(lua_State* L)
		{
			StoryThread& thread = check_story(L, "wait_until");
			luaL_checktype(L, 1, LUA_TFUNCTION);
			const f64 every = luaL_optnumber(L, 2, 1.0);
			if (every < 1.0 || every > 4294967295.0 || every != static_cast<f64>(static_cast<u32>(every)))
				luaL_error(L, "wait_until's every is a whole number of ticks from 1");
			thread.until_fn = lua_ref(L, 1);
			thread.every	= static_cast<u32>(every);
			thread.wake		= HostAccess::brains(HostAccess::of(L)).resuming_now + thread.every;
			thread.wait		= StoryThread::Wait::Until;
			return lua_yield(L, 0);
		}

		int lua_wait_for(lua_State* L)
		{
			StoryThread& thread = check_story(L, "wait_for");
			thread.event_name	= intern_name(HostAccess::of(L), luaL_checkstring(L, 1));
			thread.wait			= StoryThread::Wait::Event;
			return lua_yield(L, 0);
		}

		// --- story "x" { ... } ---

		int story_body(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			const char* name = lua_tostring(L, lua_upvalueindex(1));
			if (!HostAccess::loading(host))
				luaL_error(L, "story is declared at a module's top level, as it loads");
			luaL_checktype(L, 1, LUA_TTABLE);

			const Context context = HostAccess::loading_context(host);
			if (context != Context::Server && context != Context::Client)
				luaL_error(
					L,
					"story %s: a story waits, so it runs where nothing is replayed: scripts/server or scripts/client",
					name);

			if (HostAccess::def(host).schema != nullptr)
				return 0;

			StoryDecl decl;
			decl.name	 = String(name, &heap());
			decl.id		 = static_cast<u32>(hash_text(name));
			decl.module	 = static_cast<u32>(HostAccess::loading_module(host));
			decl.context = context;

			const Span<const StageName> stages = HostAccess::binding(host).stages();
			if (!stages.empty())
			{
				u8 first = stages[0].index;
				for (const StageName& stage : stages)
					first = std::min(first, stage.index);
				decl.stage = first;
			}

			lua_pushnil(L);
			while (lua_next(L, 1) != 0)
			{
				if (!lua_isstring(L, -2))
					luaL_error(L, "story %s: keys are names", name);
				const StringView key = lua_tostring(L, -2);
				const int value		 = lua_gettop(L);

				if (key == "prefab")
				{
					if (!lua_isstring(L, value))
						luaL_error(L, "story %s: prefab names a prefab", name);
					const ecs::Prefab* prefab = host.world().prefabs().find(lua_tostring(L, value));
					if (prefab == nullptr)
						luaL_error(L, "story %s: no prefab named '%s'", name, lua_tostring(L, value));
					decl.prefab = prefab->id;
				}
				else if (key == "stage")
				{
					const StageName* stage =
						lua_isstring(L, value) ? HostAccess::binding(host).find_stage(lua_tostring(L, value)) : nullptr;
					if (stage == nullptr)
						luaL_error(L, "story %s: no stage named '%s'", name, lua_tostring(L, value));
					decl.stage = stage->index;
				}
				else if (key == "loop")
				{
					if (!lua_isboolean(L, value))
						luaL_error(L, "story %s: loop is true or false", name);
					decl.loop = lua_toboolean(L, value) != 0;
				}
				else if (key == "run")
				{
					if (!lua_isfunction(L, value))
						luaL_error(L, "story %s: run takes a function", name);
					decl.run = lua_ref(L, value);
				}
				else if (key == "on")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "story %s: on is { event = function(e, event) end, ... }", name);
					lua_pushnil(L);
					while (lua_next(L, value) != 0)
					{
						if (!lua_isstring(L, -2) || !lua_isfunction(L, -1))
							luaL_error(L, "story %s: on is { event = function(e, event) end, ... }", name);
						decl.on.push_back({.name = intern_name(host, lua_tostring(L, -2)), .fn = lua_ref(L, -1)});
						lua_pop(L, 1);
					}
				}
				else
				{
					String keys(&heap());
					for (const StringView known : STORY_KEYS)
					{
						if (!keys.empty())
							keys += ", ";
						keys += known;
					}
					luaL_error(L, "story %s: unknown key '%s'; a story has %s", name, key.data(), keys.c_str());
				}
				lua_pop(L, 1);
			}

			if (decl.run < 0 && decl.on.empty())
				luaL_error(L, "story %s: neither run nor on: a story runs, or answers events, or both", name);
			if (decl.run < 0 && decl.loop)
				luaL_error(L, "story %s: loop without run", name);

			(void)intern_name(host, name);
			HostAccess::brains(host).stories_staging.push_back(std::move(decl));
			return 0;
		}

		int lua_story(lua_State* L)
		{
			luaL_checkstring(L, 1);
			lua_pushvalue(L, 1);
			lua_pushcclosure(L, story_body, "story", 1);
			return 1;
		}

		/** shown(e): the entity as a client shows it; the same handle, for the editor's types. */
		int lua_shown(lua_State* L)
		{
			(void)check_entity(L, 1);
			lua_pushvalue(L, 1);
			return 1;
		}
	}

	void release_story(lua_State* L, StoryDecl& decl) noexcept
	{
		if (decl.run >= 0)
			lua_unref(L, decl.run);
		for (const ShowRule& rule : decl.on)
			if (rule.fn >= 0)
				lua_unref(L, rule.fn);
		decl.run = -1;
		decl.on.clear();
	}

	void sweep_threads(Host& host) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		Vector<u64> gone(&heap());
		for (auto& [key, thread] : brains.threads)
		{
			if (thread.sweep == StoryThread::Sweep::None)
				continue;
			drop(host.state(), thread);
			if (thread.sweep == StoryThread::Sweep::Erase)
			{
				gone.push_back(key);
				continue;
			}
			thread.done	 = true;
			thread.sweep = StoryThread::Sweep::None;
		}
		for (const u64 key : gone)
			brains.threads.erase(key);
	}

	void drop_story_threads(Host& host, u32 story) noexcept
	{
		// A story's module reloaded: every run of it starts over on the new text.
		for (auto& [key, thread] : HostAccess::brains(host).threads)
			if (thread.story == story)
				thread.sweep = StoryThread::Sweep::Erase;
	}

	void drop_entity_stories(Host& host, ecs::Entity entity, u32 story, bool restart) noexcept
	{
		for (auto& [key, thread] : HostAccess::brains(host).threads)
			if (thread.entity == entity && (story == 0 || thread.story == story))
				thread.sweep = restart ? StoryThread::Sweep::Erase : StoryThread::Sweep::Rest;
	}

	int entity_start(lua_State* L, Host& host, ecs::Entity entity)
	{
		check_context(L, host, ContextMask::Server | ContextMask::Client, "e:start");
		const char* name = luaL_checkstring(L, 2);
		const u32 id	 = static_cast<u32>(intern_name(host, name));
		if (decl_of(HostAccess::brains(host), id) == nullptr)
			luaL_error(L, "e:start: no story named '%s'", name);

		// Into a free slot of its Story; one it runs already, or one its prefab gives it that was stopped, starts over.
		drop_entity_stories(host, entity, id, true);
		Story& story = host.world().registry.get_or_emplace<Story>(entity);
		for (const u32 slot : story.slots)
			if (slot == id)
				return 0;
		const StoryDecl* decl	  = decl_of(HostAccess::brains(host), id);
		const ecs::PrefabRef* ref = host.world().registry.try_get<ecs::PrefabRef>(entity);
		if (decl != nullptr && ref != nullptr && decl->prefab == ref->id)
			return 0;
		for (u32& slot : story.slots)
		{
			if (slot == 0)
			{
				slot = id;
				return 0;
			}
		}
		luaL_error(L, "e:start: Entity(%u) runs %u stories already", static_cast<unsigned>(entt::to_integral(entity)),
				   static_cast<unsigned>(Story::SLOTS));
	}

	int entity_stop(lua_State* L, Host& host, ecs::Entity entity)
	{
		check_context(L, host, ContextMask::Server | ContextMask::Client, "e:stop");
		// Its coroutine goes at the next sweep; one its prefab gives it stays stopped until e:start() asks again.
		const u32 id = lua_isnoneornil(L, 2) ? 0 : static_cast<u32>(hash_text(luaL_checkstring(L, 2)));
		if (Story* story = host.world().registry.try_get<Story>(entity))
			for (u32& slot : story->slots)
				if (id == 0 || slot == id)
					slot = 0;
		drop_entity_stories(host, entity, id, false);
		return 0;
	}

	void story_report(Host& host, ecs::Entity entity, Vector<StoryReport>& out) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		for (const auto& [key, thread] : brains.threads)
		{
			if (thread.entity != entity)
				continue;
			const StoryDecl* decl = decl_of(brains, thread.story);
			StoryReport report;
			report.story   = decl != nullptr ? StringView(decl->name) : StringView("?");
			report.line	   = thread.line;
			report.waiting = thread.failed ? "failed"
							 : thread.done ? (decl != nullptr && decl->run < 0 ? "on" : "done")
							 : thread.sweep != StoryThread::Sweep::None ? "stopping"
																		: wait_name(thread.wait);
			report.wake =
				thread.wait == StoryThread::Wait::Ticks || thread.wait == StoryThread::Wait::Until ? thread.wake : 0;
			report.event =
				thread.wait == StoryThread::Wait::Event ? name_text(host.state(), thread.event_name) : StringView();
			out.push_back(report);
		}
	}

	void install_stories(lua_State* L, Host& host)
	{
		(void)host;
		lua_pushcfunction(L, lua_story, "story");
		lua_setglobal(L, "story");
		lua_pushcfunction(L, lua_wait, "wait");
		lua_setglobal(L, "wait");
		lua_pushcfunction(L, lua_wait_until, "wait_until");
		lua_setglobal(L, "wait_until");
		lua_pushcfunction(L, lua_wait_for, "wait_for");
		lua_setglobal(L, "wait_for");
		lua_pushcfunction(L, lua_shown, "shown");
		lua_setglobal(L, "shown");
	}
}
