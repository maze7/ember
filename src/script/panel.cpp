#include "internal.h"

#include <ember/core/hash.h>

#include <algorithm>

/**
 * Panels: a client script's debug UI. `panel "name" { state = { ... }, frame = function(state) ... end }`
 * declares one; the game's debug UI draws it in a window of its own and runs its frame inside, once a frame,
 * on the main thread and outside the world's run, through Host::run_panel(). The frame is handed its state, a
 * table a reload keeps the values of: a checkbox stays ticked across a save. What a frame draws with is the
 * game's: Ember::ScriptUi's `ui` library, when the game binds it. A frame reads the world and draws; it never
 * writes the game.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		int panel_body(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			const char* name = lua_tostring(L, lua_upvalueindex(1));
			if (!HostAccess::loading(host))
				luaL_error(L, "panel is declared at a module's top level, as it loads");
			luaL_checktype(L, 1, LUA_TTABLE);
			if (HostAccess::loading_context(host) != Context::Client)
				luaL_error(L, "panel %s: a panel is debug UI on a client, so it is declared in scripts/client", name);
			if (HostAccess::def(host).schema != nullptr)
				return 0;

			PanelDecl decl;
			decl.name	= String(name, &heap());
			decl.id		= hash_text(name);
			decl.module = static_cast<u32>(HostAccess::loading_module(host));

			lua_pushnil(L);
			while (lua_next(L, 1) != 0)
			{
				const StringView key = lua_isstring(L, -2) ? StringView(lua_tostring(L, -2)) : StringView();
				if (key == "frame")
				{
					if (!lua_isfunction(L, -1))
						luaL_error(L, "panel %s: frame takes a function", name);
					decl.frame = lua_ref(L, -1);
				}
				else if (key == "state")
				{
					if (!lua_istable(L, -1))
						luaL_error(L, "panel %s: state takes a table", name);
					decl.state = lua_ref(L, -1);
				}
				else
				{
					luaL_error(L, "panel %s: unknown key '%s'; a panel has state and frame", name, key.data());
				}
				lua_pop(L, 1);
			}
			if (decl.frame < 0)
				luaL_error(L, "panel %s: no frame", name);
			if (decl.state < 0)
			{
				lua_newtable(L);
				decl.state = lua_ref(L, -1);
				lua_pop(L, 1);
			}
			HostAccess::brains(host).panels_staging.push_back(std::move(decl));
			return 0;
		}

		int lua_panel(lua_State* L)
		{
			luaL_checkstring(L, 1);
			lua_pushvalue(L, 1);
			lua_pushcclosure(L, panel_body, "panel", 1);
			return 1;
		}
	}

	void release_panel(lua_State* L, PanelDecl& decl) noexcept
	{
		if (decl.frame >= 0)
			lua_unref(L, decl.frame);
		if (decl.state >= 0)
			lua_unref(L, decl.state);
		decl.frame = -1;
		decl.state = -1;
	}

	void keep_panel_state(lua_State* L, const PanelDecl& old, PanelDecl& fresh) noexcept
	{
		if (old.state < 0 || fresh.state < 0)
			return;
		lua_getref(L, fresh.state);
		lua_getref(L, old.state);
		lua_pushnil(L);
		while (lua_next(L, -2) != 0)
		{
			lua_pushvalue(L, -2); // the key, again
			lua_insert(L, -2);	  // under the value
			lua_rawset(L, -5);	  // into the fresh table
		}
		lua_pop(L, 2);
	}

	void sort_panels(Host& host) noexcept
	{
		Vector<PanelDecl>& panels = HostAccess::brains(host).panels;
		std::sort(panels.begin(), panels.end(), [](const PanelDecl& a, const PanelDecl& b) { return a.name < b.name; });
	}

	bool run_panel(Host& host, u32 index) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		if (index >= brains.panels.size() || host.state() == nullptr)
			return false;
		PanelDecl& panel = brains.panels[index];
		if (panel.disabled)
			return false;

		lua_State* thread = HostAccess::thread_of(host, panel.module);
		lua_settop(thread, 0);
		lua_getref(thread, panel.frame);
		lua_getref(thread, panel.state);
		brains.netids_built = false;
		brains.in_panel		= true;
		HostAccess::begin_call(host, Context::Client);
		const int status = lua_pcall(thread, 1, 0, 0);
		HostAccess::end_call(host);
		brains.in_panel = false;
		++HostAccess::stats(host).calls;
		if (status == LUA_OK)
			return true;

		String message("panel ", &heap());
		message += panel.name;
		message += ": ";
		message += lua_tostring(thread, -1) != nullptr ? lua_tostring(thread, -1) : "error";
		HostAccess::report_lua(host, HostAccess::path_of(host, panel.module), message.c_str());
		lua_settop(thread, 0);
		panel.disabled = true;
		++HostAccess::stats(host).errors;
		return false;
	}

	void install_panels(lua_State* L, Host& host)
	{
		(void)host;
		lua_pushcfunction(L, lua_panel, "panel");
		lua_setglobal(L, "panel");
	}
}
