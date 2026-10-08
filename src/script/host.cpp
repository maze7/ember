#include "internal.h"

#include <ember/core/filesystem.h>
#include <ember/core/hash.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>
#include <ember/script/dmath.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string>

namespace ember::script
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		[[nodiscard]] f64 microseconds_since(Clock::time_point start) noexcept
		{
			return std::chrono::duration<f64, std::micro>(Clock::now() - start).count();
		}

		/** The most problems kept; past it, only counts of what is already known move. */
		constexpr size_t MAX_PROBLEMS = 512;

		// math's transcendental functions on the deterministic implementations: the same bits on every
		// machine, where libm's differ in the last place between C libraries.
		int math_sin(lua_State* L)
		{
			lua_pushnumber(L, dmath::sin(luaL_checknumber(L, 1)));
			return 1;
		}
		int math_cos(lua_State* L)
		{
			lua_pushnumber(L, dmath::cos(luaL_checknumber(L, 1)));
			return 1;
		}
		int math_tan(lua_State* L)
		{
			lua_pushnumber(L, dmath::tan(luaL_checknumber(L, 1)));
			return 1;
		}
		int math_asin(lua_State* L)
		{
			lua_pushnumber(L, dmath::asin(luaL_checknumber(L, 1)));
			return 1;
		}
		int math_acos(lua_State* L)
		{
			lua_pushnumber(L, dmath::acos(luaL_checknumber(L, 1)));
			return 1;
		}
		int math_atan(lua_State* L)
		{
			lua_pushnumber(L, dmath::atan(luaL_checknumber(L, 1)));
			return 1;
		}
		int math_atan2(lua_State* L)
		{
			lua_pushnumber(L, dmath::atan2(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
			return 1;
		}
		int math_exp(lua_State* L)
		{
			lua_pushnumber(L, dmath::exp(luaL_checknumber(L, 1)));
			return 1;
		}
		int math_log(lua_State* L)
		{
			const f64 x = luaL_checknumber(L, 1);
			if (lua_isnoneornil(L, 2))
				lua_pushnumber(L, dmath::log(x));
			else
				lua_pushnumber(L, dmath::log(x) / dmath::log(luaL_checknumber(L, 2)));
			return 1;
		}
		int math_pow(lua_State* L)
		{
			lua_pushnumber(L, dmath::pow(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
			return 1;
		}

		constexpr luaL_Reg DETERMINISTIC_MATH[] = {
			{"sin", math_sin},	 {"cos", math_cos},		{"tan", math_tan}, {"asin", math_asin}, {"acos", math_acos},
			{"atan", math_atan}, {"atan2", math_atan2}, {"exp", math_exp}, {"log", math_log},	{"pow", math_pow},
		};

		/** What the base library has that a sandbox does not: code from strings, environments, and the collector. */
		constexpr const char* REMOVED_GLOBALS[] = {"loadstring", "getfenv", "setfenv", "newproxy", "collectgarbage", "gcinfo"};

		/** print(...) into the engine's log: a script's trace, tab separated as Lua's is. */
		int lua_print(lua_State* L)
		{
			String text(&heap());
			const int count = lua_gettop(L);
			for (int index = 1; index <= count; ++index)
			{
				size_t length	 = 0;
				const char* part = luaL_tolstring(L, index, &length);
				if (index > 1)
					text += '\t';
				text.append(part, length);
				lua_pop(L, 1);
			}
			EMBER_INFO("[script] {}", StringView(text));
			return 0;
		}

		/**
		 * "scripts/lib/x.luau:12: message" into the file, its line and the message: the file the error was
		 * raised in, which may be a lib the system called. Without a location the message is whole and the
		 * line 0, and the path is the caller's.
		 */
		void split_location(StringView fallback, StringView message, StringView& path, u32& line, StringView& rest) noexcept
		{
			path = fallback;
			line = 0;
			rest = message;

			// The first "name:digits:" whose name has no spaces: Luau's own prefix.
			const size_t colon = message.find(".luau:");
			if (colon == StringView::npos || message.substr(0, colon).find(' ') != StringView::npos)
				return;

			const StringView after = message.substr(colon + 6);
			u32 parsed			   = 0;
			const auto result	   = std::from_chars(after.data(), after.data() + after.size(), parsed);
			if (result.ec != std::errc() || result.ptr == after.data() + after.size() || *result.ptr != ':')
				return;

			path = message.substr(0, colon + 5);
			line = parsed;
			rest = StringView(result.ptr + 1);
			while (!rest.empty() && rest.front() == ' ')
				rest.remove_prefix(1);
		}

		/** Copies every field of the table at `from` into the table at `to`; both absolute indices. */
		void copy_fields(lua_State* L, int from, int to)
		{
			lua_pushnil(L);
			while (lua_next(L, from) != 0)
			{
				lua_pushvalue(L, -2);
				lua_pushvalue(L, -2);
				lua_rawset(L, to);
				lua_pop(L, 1);
			}
		}
	}

	// --- VM callbacks -----------------------------------------------------------------------------

	void* Host::alloc(void* ud, void* ptr, size_t osize, size_t nsize) noexcept
	{
		Host& host = *static_cast<Host*>(ud);
		if (nsize == 0)
		{
			if (ptr != nullptr)
			{
				host.m_lua_bytes -= osize;
				heap().deallocate_unsized(ptr);
			}
			return nullptr;
		}

		void* block = heap().reallocate(ptr, nsize);
		if (block != nullptr)
			host.m_lua_bytes += nsize - (ptr != nullptr ? osize : 0);
		return block;
	}

	void Host::interrupt(lua_State* L, int gc)
	{
		// Called at loop edges and calls while a script runs, and from the collector, which is not a step.
		if (gc >= 0)
			return;

		Host& host = HostAccess::of(L);
		if (++host.m_steps > host.m_def.budget)
		{
			host.m_steps = 0;
			luaL_error(L, "ran for %u safepoints without returning: an endless loop? (HostDef::budget)",
					   static_cast<unsigned>(host.m_def.budget));
		}
	}

	i16 Host::useratom(lua_State* L, const char* s, size_t l)
	{
		// Asked once per interned string, the first time an atom is wanted of it: the metamethods then
		// dispatch on a small integer rather than compare names.
		const Host& host	  = HostAccess::of(L);
		const StringView name = {s, l};
		const auto it		  = host.m_atoms.find(hash_text(name));
		if (it == host.m_atoms.end())
			return -1;
		return host.m_atom_names[static_cast<size_t>(it->second)] == name ? it->second : -1;
	}

	int Host::lua_trampoline(lua_State* L)
	{
		const Function& function = *static_cast<const Function*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
		check_context(L, HostAccess::of(L), function.where, function.label != nullptr ? function.label : function.name);
		return function.call(L);
	}

	int Host::lua_now(lua_State* L)
	{
		lua_pushnumber(L, static_cast<f64>(HostAccess::of(L).m_now));
		return 1;
	}

	int Host::lua_system(lua_State* L)
	{
		Host& host = HostAccess::of(L);
		if (host.m_loading < 0)
			luaL_error(L, "system() is called at a module's top level, as it loads");

		const char* stage_name = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);

		const Module& module = host.m_modules[static_cast<size_t>(host.m_loading)];
		u8 stage			 = PRESENT;

		if (StringView(stage_name) == "Present")
		{
			if (module.context != Context::Client)
				luaL_error(L, "Present is for scripts/client; a %s script declares a simulate stage",
						   enum_name(module.context));
		}
		else
		{
			if (module.context == Context::Client)
				luaL_error(L, "a client script presents: system(\"Present\", fn)");
			if (module.context == Context::Lib)
				luaL_error(L, "a lib declares no systems; require it from a sim, server or client script");

			const StageName* found = host.m_binding.find_stage(stage_name);
			if (found == nullptr)
			{
				String names(&heap());
				for (const StageName& name : host.m_binding.stages())
				{
					if (!names.empty())
						names += ", ";
					names += name.name;
				}
				luaL_error(L, "no stage named '%s'; the stages are %s, and Present", stage_name, names.c_str());
			}
			stage = found->index;
		}

		// The schema pass wants declarations alone: a system is noted as well formed and let go.
		if (host.m_def.schema != nullptr)
			return 0;

		System system;
		system.module  = static_cast<u32>(host.m_loading);
		system.ordinal = static_cast<u32>(host.m_staging.size());
		system.stage   = stage;
		system.ref	   = lua_ref(L, 2);
		host.m_staging.push_back(system);
		return 0;
	}

	int Host::lua_require(lua_State* L)
	{
		Host& host			 = HostAccess::of(L);
		const char* required = luaL_checkstring(L, 1);

		// Whose require: the module loading now, or the one the calling function was defined in.
		Module* from = nullptr;
		if (host.m_loading >= 0)
		{
			from = &host.m_modules[static_cast<size_t>(host.m_loading)];
		}
		else
		{
			lua_Debug ar;
			if (lua_getinfo(L, 1, "s", &ar) && ar.source != nullptr && ar.source[0] == '=')
				from = host.find_module(ar.source + 1);
		}
		if (from == nullptr)
			luaL_error(L, "require() is for modules");

		const Import* import = nullptr;
		for (const Import& candidate : from->imports)
		{
			if (candidate.required == required)
			{
				import = &candidate;
				break;
			}
		}
		if (import == nullptr)
			luaL_error(L, "require(\"%s\"): not a module this file names; write \"@alias/path\" or \"./path\" as a literal",
					   required);

		// The path, not the pointer: loading may move the modules.
		String path(import->path, &heap());
		const Module* target = host.ensure_loaded(path);
		if (target == nullptr || !target->loaded)
			luaL_error(L, "require(\"%s\"): %s did not load", required, path.c_str());

		if (target->value_ref >= 0)
			lua_getref(L, target->value_ref);
		else
			lua_pushnil(L);
		return 1;
	}

	// --- Host -------------------------------------------------------------------------------------

	Host::Host(ecs::World& world, const HostDef& def) noexcept
		: m_world(world), m_def(def), m_binding(world), m_atoms(&heap()), m_atom_names(&heap()),
		  m_component_by_atom(&heap()), m_verb_by_atom(&heap()), m_sources(&heap()), m_modules(&heap()),
		  m_module_by_path(&heap()), m_systems(&heap()), m_buckets(&heap()), m_bucket_us(&heap()), m_staging(&heap()),
		  m_brains(memory::make_unique<Brains>(MemoryTag::Scripting)), m_problems(&heap())
	{
		expose_engine();
	}

	void Host::expose_engine() noexcept
	{
		// The engine's own components, when the game registered them, and every type the scripts declared:
		// exposed before the game's binding adds its own, so the definitions file has them all.
		const ecs::Components& components = m_world.components();
		if (const ecs::ComponentInfo* info = components.find<Stategraph>())
		{
			m_brains->stategraph = info;
			expose_stategraph(m_binding);
		}
		if (const ecs::ComponentInfo* info = components.find<anim::Playing>())
		{
			m_brains->playing = info;
			m_binding.expose<anim::Playing>();
		}
		if (const ecs::ComponentInfo* info = components.find<Cues>())
		{
			m_brains->cues = info;
			m_binding.expose<Cues>({.sealed = true}); // for prefabs: `Cues = {}`; the host writes it
		}
		if (const ecs::ComponentInfo* info = components.find<Shown>())
			m_brains->shown = info;
		if (const ecs::ComponentInfo* info = components.find<Modifiers>())
		{
			m_brains->modifiers_info = info;
			m_binding.expose<Modifiers>({.sealed = true}); // `Modifiers = {}`; e:inflict() and e:cure() write it
		}
		if (components.find<Trigger>() != nullptr)
			m_binding.expose<Trigger>();
		for (const ecs::ComponentInfo& info : components.all())
			if (info.dynamic)
				m_binding.expose_dynamic(info);
	}

	Host::~Host() noexcept
	{
		if (m_L != nullptr)
			lua_close(m_L);
	}

	i16 Host::atom(StringView name) noexcept
	{
		const u64 key = hash_text(name);
		if (const auto it = m_atoms.find(key); it != m_atoms.end())
		{
			EMBER_ASSERT(m_atom_names[static_cast<size_t>(it->second)] == name && "two names with one hash");
			return it->second;
		}

		EMBER_ASSERT(m_atom_names.size() < 32767 && "more names than atoms");
		const auto id = static_cast<i16>(m_atom_names.size());
		m_atom_names.push_back(String(name, &heap()));
		m_atoms.insert_or_assign(key, id);
		return id;
	}

	void Host::make_state() noexcept
	{
		// Every name the metamethods dispatch on gets its atom first, before the VM makes any string.
		for (Exposed& exposed : m_binding.m_exposed)
		{
			if (exposed.info == nullptr)
				continue;
			exposed.atom = atom(exposed.info->name);
			for (Field& field : exposed.fields)
				field.atom = atom(field.name);
			for (Method& method : exposed.methods)
				method.atom = atom(method.name);
		}

		const i16 verbs[static_cast<size_t>(Verb::Count)] = {
			atom("has"),	 atom("add"),	atom("remove"),	  atom("destroy"), atom("id"),	   atom("play"),
			atom("event"),	 atom("flash"), atom("overlay"),  atom("squash"),  atom("lean"),   atom("hold"),
			atom("sound"),	 atom("mine"),	atom("start"),	  atom("stop"),	   atom("exists"), atom("strike"),
			atom("inflict"), atom("cure"),	atom("modifier"), atom("stat"),	   atom("prefab")};
		static_assert(std::size(verbs) == static_cast<size_t>(Verb::Count));
		m_brains->atom_rng								   = atom("rng");
		if (!m_binding.stages().empty())
		{
			m_brains->first_stage = m_binding.stages()[0].index;
			for (const StageName& stage : m_binding.stages())
				m_brains->first_stage = std::min(m_brains->first_stage, stage.index);
		}
		m_brains->atom_state = atom("state");
		Vector<i16> game_verbs(&heap());
		for (const Function& method : m_binding.m_entity_methods)
			game_verbs.push_back(atom(method.name));

		m_component_by_atom.assign(m_atom_names.size(), ecs::NO_COMPONENT);
		m_verb_by_atom.assign(m_atom_names.size(), static_cast<i16>(Verb::None));
		for (const Exposed& exposed : m_binding.m_exposed)
			if (exposed.info != nullptr)
				m_component_by_atom[static_cast<size_t>(exposed.atom)] = exposed.info->id;
		for (size_t i = 0; i < std::size(verbs); ++i)
			m_verb_by_atom[static_cast<size_t>(verbs[i])] = static_cast<i16>(i);
		for (size_t i = 0; i < game_verbs.size(); ++i)
			m_verb_by_atom[static_cast<size_t>(game_verbs[i])] = static_cast<i16>(static_cast<size_t>(Verb::Count) + i);

		m_L						= lua_newstate(&alloc, this);
		lua_Callbacks* callbacks = lua_callbacks(m_L);
		callbacks->userdata		= this;
		callbacks->interrupt	= &interrupt;
		callbacks->useratom		= &useratom;

		// Luau's libraries, less os, io and debug, which never open; coroutine is kept aside for clients.
		for (lua_CFunction open : {luaopen_base, luaopen_string, luaopen_table, luaopen_bit32, luaopen_buffer,
								   luaopen_utf8, luaopen_vector, luaopen_math, luaopen_coroutine})
		{
			lua_pushcfunction(m_L, open, nullptr);
			lua_call(m_L, 0, 0);
		}

		for (const char* name : REMOVED_GLOBALS)
		{
			lua_pushnil(m_L);
			lua_setglobal(m_L, name);
		}
		lua_pushcfunction(m_L, lua_print, "print");
		lua_setglobal(m_L, "print");

		lua_getglobal(m_L, "coroutine");
		lua_setreadonly(m_L, -1, true);
		m_ref_coroutine = lua_ref(m_L, -1);
		lua_pop(m_L, 1);
		lua_pushnil(m_L);
		lua_setglobal(m_L, "coroutine");

		// math as everyone has it: deterministic, with no random; and the copy that keeps random() for
		// scripts that run on one machine only.
		lua_getglobal(m_L, "math");
		const int math_index = lua_gettop(m_L);
		for (const luaL_Reg& function : DETERMINISTIC_MATH)
		{
			lua_pushcfunction(m_L, function.func, function.name);
			lua_setfield(m_L, math_index, function.name);
		}
		lua_newtable(m_L);
		copy_fields(m_L, math_index, lua_gettop(m_L));
		lua_setreadonly(m_L, -1, true);
		m_ref_math_random = lua_ref(m_L, -1);
		lua_pop(m_L, 1);
		lua_pushnil(m_L);
		lua_setfield(m_L, math_index, "random");
		lua_pushnil(m_L);
		lua_setfield(m_L, math_index, "randomseed");
		lua_pop(m_L, 1);

		install_handles(m_L, *this);
		install_world(m_L, *this);
		install_physics(m_L, *this);
		install_declarations(m_L, *this);
		install_stategraphs(m_L, *this);
		install_shows(m_L, *this);
		install_stories(m_L, *this);
		install_modifiers(m_L, *this);
		install_panels(m_L, *this);
		install_binding();

		// Everything a script sees is read only from here; each module gets a global table of its own over it.
		luaL_sandbox(m_L);
		EMBER_ASSERT(lua_gettop(m_L) == 0);
	}

	void Host::push_function(const Function& function) noexcept
	{
		if (function.where == ContextMask::All)
		{
			lua_pushcfunction(m_L, function.call, function.name);
			return;
		}

		// Checked on the way in: the record lasts as long as the binding, which lasts as long as the host.
		lua_pushlightuserdata(m_L, const_cast<Function*>(&function));
		lua_pushcclosure(m_L, lua_trampoline, function.name, 1);
	}

	void Host::install_binding() noexcept
	{
		// Component types by their names, their storages made so a query can walk them from the start.
		for (const Exposed& exposed : m_binding.m_exposed)
		{
			if (exposed.info == nullptr)
				continue;
			exposed.info->assure(*exposed.info, m_world.registry);
			push_component_type(m_L, exposed.info->id);
			lua_setglobal(m_L, exposed.info->name.data());
		}
		(void)m_world.registry.storage<ecs::Simulated>();

		for (const Library& library : m_binding.m_libraries)
		{
			lua_createtable(m_L, 0, static_cast<int>(library.functions.size()));
			for (const Function& function : library.functions)
			{
				push_function(function);
				lua_setfield(m_L, -2, function.name);
			}
			lua_setglobal(m_L, library.name.c_str());
		}

		for (const Function& function : m_binding.m_globals)
		{
			push_function(function);
			lua_setglobal(m_L, function.name);
		}

		lua_getglobal(m_L, "world");
		for (const Function& function : m_binding.m_world_functions)
		{
			push_function(function);
			lua_setfield(m_L, -2, function.name);
		}
		lua_pop(m_L, 1);

		for (const Enumeration& enumeration : m_binding.m_enumerations)
		{
			lua_createtable(m_L, 0, static_cast<int>(enumeration.values.size()));
			for (const Enumeration::Value& value : enumeration.values)
			{
				lua_pushnumber(m_L, value.value);
				lua_setfield(m_L, -2, value.name.c_str());
			}
			lua_setglobal(m_L, enumeration.name.c_str());
		}

		for (const Constant& constant : m_binding.m_constants)
		{
			lua_pushnumber(m_L, constant.value);
			lua_setglobal(m_L, constant.name.c_str());
		}

		for (const Userdata& userdata : m_binding.m_userdata)
		{
			lua_newtable(m_L);
			lua_pushstring(m_L, userdata.type_name.c_str());
			lua_setfield(m_L, -2, "__type");
			if (userdata.namecall != nullptr)
			{
				lua_pushcfunction(m_L, userdata.namecall, "__namecall");
				lua_setfield(m_L, -2, "__namecall");
			}
			lua_setreadonly(m_L, -1, true);
			lua_setuserdatametatable(m_L, userdata.tag);
		}

		lua_pushcfunction(m_L, lua_system, "system");
		lua_setglobal(m_L, "system");
		lua_pushcfunction(m_L, lua_now, "now");
		lua_setglobal(m_L, "now");
		lua_pushcfunction(m_L, lua_require, "require");
		lua_setglobal(m_L, "require");
	}

	Host::Module* Host::find_module(StringView path) noexcept
	{
		const auto it = m_module_by_path.find(hash_text(path));
		if (it == m_module_by_path.end())
			return nullptr;
		Module& module = m_modules[it->second];
		return module.path == path ? &module : nullptr;
	}

	Host::Module* Host::ensure_loaded(StringView path) noexcept
	{
		const auto it = m_sources.find(hash_text(path));
		Pending* pending = it != m_sources.end() && it->second.source.path == path ? &it->second : nullptr;

		Module* module = find_module(path);
		if (module != nullptr && module->loaded && (pending == nullptr || !pending->dirty))
			return module;
		if (pending == nullptr)
			return module; // never given: nothing to load it from

		if (module != nullptr && module->loading)
		{
			report(path, 0, Severity::Error, "required by a module it requires: a cycle");
			return nullptr;
		}

		(void)load_module(*pending);
		return find_module(path);
	}

	bool Host::load_module(Pending& pending) noexcept
	{
		const Source& source = pending.source;
		pending.dirty		 = false;

		// Its place: a module reloads into its own, a new one is appended.
		u32 index;
		if (const Module* existing = find_module(source.path); existing != nullptr)
		{
			index = static_cast<u32>(existing - m_modules.data());
		}
		else
		{
			index = static_cast<u32>(m_modules.size());
			Module& made = m_modules.emplace_back();
			made.path	 = String(source.path, &heap());
			m_module_by_path.insert_or_assign(hash_text(source.path), index);
		}

		// What was wrong with the file is wrong no more, until this version says otherwise.
		std::erase_if(m_problems, [&](const Problem& problem) { return problem.path == source.path; });
		for (const Problem& lint : source.lints)
			report(lint.path, lint.line, lint.severity, lint.message);

		if (!source.loadable())
		{
			if (source.bytecode.empty())
				report(source.path, 0, Severity::Error, "did not compile");
			return false;
		}

		// A thread of its own, with a global table of its own over the shared, read only one, and the
		// libraries its context may have.
		lua_State* thread	 = lua_newthread(m_L);
		const int thread_ref = lua_ref(m_L, -1);
		lua_pop(m_L, 1);
		luaL_sandboxthread(thread);
		if (source.context == Context::Client)
		{
			lua_getref(thread, m_ref_coroutine);
			lua_setfield(thread, LUA_GLOBALSINDEX, "coroutine");
		}
		if (source.context == Context::Client || source.context == Context::Server)
		{
			lua_getref(thread, m_ref_math_random);
			lua_setfield(thread, LUA_GLOBALSINDEX, "math");
		}

		String chunkname("=", &heap());
		chunkname += source.path;
		if (luau_load(thread, chunkname.c_str(), reinterpret_cast<const char*>(source.bytecode.data()),
					  source.bytecode.size(), 0) != 0)
		{
			report_lua(source.path, lua_tostring(thread, -1));
			lua_unref(m_L, thread_ref);
			return false;
		}

		// Its imports are the new text's from here, so a require() its top level makes resolves; the
		// old ones come back if it fails.
		Vector<Import> old_imports(std::move(m_modules[index].imports));
		m_modules[index].imports = source.imports;
		m_modules[index].context = source.context;

		// The top level runs with system() open and the world shut. Whatever it returns is what
		// require() gives. A require() in it may load another module first, so everything that says
		// which module is loading is saved around it.
		const i32 outer_loading		   = m_loading;
		const Context outer_context	   = m_context;
		ecs::Commands* outer_commands  = m_commands;
		Vector<System> outer_staging(std::move(m_staging));
		Vector<Graph> outer_graphs(std::move(m_brains->staging));
		Vector<Extras> outer_extras(std::move(m_brains->extras_staging));
		Vector<PrefabShow> outer_shows(std::move(m_brains->prefab_shows_staging));
		Vector<StoryDecl> outer_stories(std::move(m_brains->stories_staging));
		Vector<ModifierDecl> outer_modifiers(std::move(m_brains->modifiers_staging));
		m_brains->modifiers_staging.clear();
		Vector<PanelDecl> outer_panels(std::move(m_brains->panels_staging));
		m_brains->panels_staging.clear();
		m_staging.clear();
		m_brains->staging.clear();
		m_brains->extras_staging.clear();
		m_brains->prefab_shows_staging.clear();
		m_brains->stories_staging.clear();
		m_modules[index].loading = true;
		m_loading				 = static_cast<i32>(index);
		m_context				 = Context::Count;
		m_commands				 = nullptr;
		m_steps					 = 0;

		const int status = lua_pcall(thread, 0, 1, 0);

		Module& module	= m_modules[index]; // the vector may have grown
		module.loading	= false;
		m_loading		= outer_loading;
		m_context		= outer_context;
		m_commands		= outer_commands;
		Vector<System> staged(std::move(m_staging));
		Vector<Graph> graphs(std::move(m_brains->staging));
		Vector<Extras> extras(std::move(m_brains->extras_staging));
		Vector<PrefabShow> shows(std::move(m_brains->prefab_shows_staging));
		Vector<StoryDecl> stories(std::move(m_brains->stories_staging));
		Vector<ModifierDecl> modifiers(std::move(m_brains->modifiers_staging));
		m_brains->modifiers_staging = std::move(outer_modifiers);
		Vector<PanelDecl> panels(std::move(m_brains->panels_staging));
		m_brains->panels_staging	   = std::move(outer_panels);
		m_staging					   = std::move(outer_staging);
		m_brains->staging			   = std::move(outer_graphs);
		m_brains->extras_staging	   = std::move(outer_extras);
		m_brains->prefab_shows_staging = std::move(outer_shows);
		m_brains->stories_staging	   = std::move(outer_stories);

		// Its graphs, finished now that every g.state under them has run: one with a mistake refuses the module.
		String why(&heap());
		bool finished = status == LUA_OK;
		if (finished)
		{
			Vector<String> warnings(&heap());
			for (Graph& graph : graphs)
			{
				if (!finish_graph(m_L, *this, graph, Span<Graph>(graphs.data(), graphs.size()), why, warnings))
				{
					finished = false;
					break;
				}

				// A graph that extends one of another module's copies it as it loads: requiring that module
				// brings this one back when it is saved.
				if (const Graph* base = graph.extends.empty() ? nullptr : m_brains->graph(graph_id(graph.extends));
					base != nullptr && base->module != index)
				{
					bool required = false;
					for (const Import& import : source.imports)
						required = required || import.path == m_modules[base->module].path;
					if (!required)
						warnings.push_back("stategraph " + graph.name + " extends " + graph.extends + ", from " +
										   m_modules[base->module].path +
										   ": require it, so a save of it reloads this one");
				}
			}
			for (const String& warning : warnings)
				report(source.path, 0, Severity::Warning, warning);
		}

		if (!finished)
		{
			if (status != LUA_OK)
				report_lua(source.path, lua_tostring(thread, -1));
			else
				report(source.path, 0, Severity::Error, why);
			for (const System& system : staged)
				lua_unref(m_L, system.ref);
			for (Graph& graph : graphs)
				release_graph(m_L, graph);
			for (PrefabShow& show : shows)
				release_show(m_L, show.show);
			for (StoryDecl& story : stories)
				release_story(m_L, story);
			for (ModifierDecl& modifier : modifiers)
				release_modifier(m_L, modifier);
			for (PanelDecl& panel : panels)
				release_panel(m_L, panel);
			lua_unref(m_L, thread_ref);
			if (module.loaded)
			{
				module.imports = std::move(old_imports);
				module.context = Context::Count;
			}
			return false;
		}

		// A panel keeps the values its state held, from the version about to go or another module's.
		for (PanelDecl& panel : panels)
			for (const PanelDecl& known : m_brains->panels)
				if (known.id == panel.id)
					keep_panel_state(m_L, known, panel);

		// The new version takes the old one's place: its thread, its value and its systems.
		unload_module(index);
		module.thread	  = thread;
		module.thread_ref = thread_ref;
		module.value_ref  = lua_isnil(thread, -1) ? -1 : lua_ref(thread, -1);
		lua_pop(thread, 1);
		module.context	 = source.context;
		module.text_hash = source.text_hash;
		module.loaded	 = true;
		for (const System& system : staged)
			m_systems.push_back(system);
		for (Graph& graph : graphs)
		{
			// A graph of the same name from another module gives way: the last loaded wins, and says so.
			std::erase_if(m_brains->graphs,
						  [&](Graph& known)
						  {
							  if (known.id != graph.id)
								  return false;
							  if (known.module != graph.module)
								  report(source.path, 0, Severity::Warning,
										 "stategraph " + graph.name + " is declared by " + m_modules[known.module].path + " too");
							  release_graph(m_L, known);
							  return true;
						  });
			m_brains->graphs.push_back(std::move(graph));
		}
		for (Extras& record : extras)
		{
			std::erase_if(m_brains->extras, [&](const Extras& known) { return known.prefab == record.prefab; });
			m_brains->extras.push_back(std::move(record));
		}
		for (PrefabShow& show : shows)
		{
			std::erase_if(m_brains->prefab_shows,
						  [&](PrefabShow& known)
						  {
							  if (known.prefab != show.prefab)
								  return false;
							  if (known.module != show.module)
								  report(source.path, 0, Severity::Warning,
										 "show " + String(m_world.prefabs()[show.prefab].name, &heap()) +
											 " is declared by " + m_modules[known.module].path + " too");
							  release_show(m_L, known.show);
							  return true;
						  });
			m_brains->prefab_shows.push_back(std::move(show));
		}
		for (StoryDecl& story : stories)
		{
			// A story of the same name from another module gives way, as a graph does; its threads start over
			// on the new text either way.
			std::erase_if(m_brains->stories,
						  [&](StoryDecl& known)
						  {
							  if (known.id != story.id)
								  return false;
							  if (known.module != story.module)
								  report(source.path, 0, Severity::Warning,
										 "story " + story.name + " is declared by " + m_modules[known.module].path +
											 " too");
							  release_story(m_L, known);
							  return true;
						  });
			drop_story_threads(*this, story.id);
			m_brains->stories.push_back(std::move(story));
		}
		for (ModifierDecl& modifier : modifiers)
		{
			// The same: the last loaded wins. What entities carry stays on them, under the new declaration.
			std::erase_if(m_brains->modifiers,
						  [&](ModifierDecl& known)
						  {
							  if (known.id != modifier.id)
								  return false;
							  if (known.module != modifier.module)
								  report(source.path, 0, Severity::Warning,
										 "modifier " + modifier.name + " is declared by " +
											 m_modules[known.module].path + " too");
							  release_modifier(m_L, known);
							  return true;
						  });
			m_brains->modifiers.push_back(std::move(modifier));
		}
		for (PanelDecl& panel : panels)
		{
			std::erase_if(m_brains->panels,
						  [&](PanelDecl& known)
						  {
							  if (known.id != panel.id)
								  return false;
							  if (known.module != panel.module)
								  report(source.path, 0, Severity::Warning,
										 "panel " + panel.name + " is declared by " + m_modules[known.module].path +
											 " too");
							  release_panel(m_L, known);
							  return true;
						  });
			m_brains->panels.push_back(std::move(panel));
		}
		sort_panels(*this);
		return true;
	}

	void Host::unload_module(u32 index) noexcept
	{
		Module& module = m_modules[index];
		if (module.thread_ref >= 0)
			lua_unref(m_L, module.thread_ref);
		if (module.value_ref >= 0)
			lua_unref(m_L, module.value_ref);
		module.thread	  = nullptr;
		module.thread_ref = -1;
		module.value_ref  = -1;
		module.loaded	  = false;

		std::erase_if(m_systems,
					  [&](const System& system)
					  {
						  if (system.module != index)
							  return false;
						  lua_unref(m_L, system.ref);
						  return true;
					  });
		std::erase_if(m_brains->graphs,
					  [&](Graph& graph)
					  {
						  if (graph.module != index)
							  return false;
						  release_graph(m_L, graph);
						  return true;
					  });
		std::erase_if(m_brains->extras, [&](const Extras& extras) { return extras.module == index; });
		std::erase_if(m_brains->prefab_shows,
					  [&](PrefabShow& show)
					  {
						  if (show.module != index)
							  return false;
						  release_show(m_L, show.show);
						  return true;
					  });
		std::erase_if(m_brains->stories,
					  [&](StoryDecl& story)
					  {
						  if (story.module != index)
							  return false;
						  drop_story_threads(*this, story.id);
						  release_story(m_L, story);
						  return true;
					  });
		std::erase_if(m_brains->modifiers,
					  [&](ModifierDecl& modifier)
					  {
						  if (modifier.module != index)
							  return false;
						  release_modifier(m_L, modifier);
						  return true;
					  });
		std::erase_if(m_brains->panels,
					  [&](PanelDecl& panel)
					  {
						  if (panel.module != index)
							  return false;
						  release_panel(m_L, panel);
						  return true;
					  });
	}

	void Host::reload(Span<const Source> sources) noexcept
	{
		EMBER_PROFILE_SCOPE_C("script reload", PROFILE_COLOR_GAMEPLAY);

		if (m_L == nullptr)
			make_state();
		m_brains->retuned.clear();
		m_brains->netids_built = false;

		// All of them known first, so a require() of one further down the batch loads it then and there.
		for (const Source& source : sources)
		{
			Pending& pending = m_sources[hash_text(source.path)];
			pending.source	 = source;
			pending.dirty	 = true;
		}

		for (const Source& source : sources)
		{
			const auto it = m_sources.find(hash_text(source.path));
			if (it != m_sources.end() && it->second.dirty)
				(void)load_module(it->second);
		}

		rebuild_order();
		gather_shown_names(*this);
	}

	void Host::rebuild_order() noexcept
	{
		// Each stage's systems in path order, then declaration order: the same on every machine with
		// the same scripts, whatever order they arrived in.
		u32 stages = 0;
		for (const StageName& stage : m_binding.stages())
			stages = std::max<u32>(stages, static_cast<u32>(stage.index) + 1);

		m_buckets.assign(stages + 1, Vector<u32>(&heap()));
		m_bucket_us.assign(stages + 1, 0.0);

		Vector<u32> order(&heap());
		for (u32 i = 0; i < m_systems.size(); ++i)
			order.push_back(i);
		std::sort(order.begin(), order.end(),
				  [&](u32 a, u32 b)
				  {
					  const System& x = m_systems[a];
					  const System& y = m_systems[b];
					  if (x.module != y.module)
						  return m_modules[x.module].path < m_modules[y.module].path;
					  return x.ordinal < y.ordinal;
				  });
		for (const u32 i : order)
		{
			const System& system = m_systems[i];
			const u32 bucket	 = system.stage == PRESENT ? stages : std::min<u32>(system.stage, stages);
			m_buckets[bucket].push_back(i);
		}

		// The hash: every loaded module's text, in path order.
		Vector<const Module*> loaded(&heap());
		for (const Module& module : m_modules)
			if (module.loaded)
				loaded.push_back(&module);
		std::sort(loaded.begin(), loaded.end(), [](const Module* a, const Module* b) { return a->path < b->path; });
		m_hash = HASH_SEED;
		for (const Module* module : loaded)
			m_hash = hash_value(module->text_hash, m_hash);
	}

	void Host::run_bucket(u32 bucket) noexcept
	{
		if (bucket >= m_buckets.size())
			return;

		const auto start = Clock::now();
		for (const u32 index : m_buckets[bucket])
		{
			System& system = m_systems[index];
			if (system.disabled)
				continue;

			// A server script's system never runs in a client's world, nor a client's in a server's.
			Module& module = m_modules[system.module];
			if (!runs_in(module.context, m_world.role()))
				continue;
			m_context = module.context;
			m_steps	  = 0;

			lua_State* thread = module.thread;
			lua_settop(thread, 0);
			lua_getref(thread, system.ref);

			const auto began = Clock::now();
			const int status = lua_pcall(thread, 0, 0, 0);
			system.us		 = microseconds_since(began);
			++m_stats.calls;

			if (status != LUA_OK)
			{
				// Off until its module reloads: a rule that fails every tick would say so every tick.
				report_lua(module.path, lua_tostring(thread, -1));
				lua_settop(thread, 0);
				system.disabled = true;
				++m_stats.errors;
			}
		}
		m_context = Context::Count;
		m_bucket_us[bucket] += microseconds_since(start);
	}

	void Host::simulate(u8 stage, ecs::Commands& commands) noexcept
	{
		if (m_L == nullptr)
			return;

		EMBER_PROFILE_SCOPE_C("script simulate", PROFILE_COLOR_GAMEPLAY);
		m_brains->netids_built = false;
		settle_pending_refs(*this);
		m_commands = &commands;
		run_events(stage);
		run_bucket(stage);
		m_commands = nullptr;
		lua_gc(m_L, LUA_GCSTEP, static_cast<int>(m_def.gc_step_kb));
	}

	void Host::set_present(f64 predicted, f64 interpolated) noexcept
	{
		m_brains->predicted	   = predicted;
		m_brains->interpolated = interpolated;
		m_brains->presenting   = true;
	}

	void Host::present(ecs::Commands& commands) noexcept
	{
		if (m_L == nullptr || m_buckets.empty())
			return;

		EMBER_PROFILE_SCOPE_C("script present", PROFILE_COLOR_GAMEPLAY);
		m_brains->netids_built = false;
		m_commands = &commands;
		m_bucket_us.back() = 0.0;
		const auto start	   = Clock::now();
		run_shows(*this);
		m_stats.present_shows_us = microseconds_since(start);
		m_brains->running		 = true;
		run_stories(*this, 0, true);
		m_brains->running = false;
		for (Event& raised : m_brains->raised)
			m_brains->events.push_back(raised);
		m_brains->raised.clear();
		run_bucket(static_cast<u32>(m_buckets.size() - 1));
		m_commands = nullptr;
		lua_gc(m_L, LUA_GCSTEP, static_cast<int>(m_def.gc_step_kb));
	}

	void Host::run_events(u8 stage) noexcept
	{
		// The graphs, then the stories, each answering the events raised for it; what nobody took in two
		// ticks is dropped, and what the handlers raised waits for the next point.
		const auto start = Clock::now();
		Brains& brains	 = *m_brains;
		brains.running	 = true;
		run_modifiers(*this, stage);
		run_stategraphs(*this, stage);
		run_stories(*this, stage, false);
		brains.running = false;
		for (Event& raised : brains.raised)
			brains.events.push_back(raised);
		brains.raised.clear();
		std::erase_if(brains.events, [&](const Event& event) { return m_now - event.tick >= 2; });
		if (stage < m_bucket_us.size())
			m_bucket_us[stage] = microseconds_since(start);
	}

	void Host::event(ecs::Entity entity, StringView name, ecs::Entity source, f32 value) noexcept
	{
		Event event;
		event.target = entity;
		event.source = source;
		event.name	 = hash_text(name);
		event.tick	 = m_now;
		event.value	 = value;
		event.serial = ++m_brains->event_serial;
		if (m_brains->running)
			m_brains->raised.push_back(event);
		else
			m_brains->events.push_back(event);
		++m_stats.events;

		// Clients that show it hear of it: through the entity's cues from a server, which replicate; on a
		// client, for what it predicts itself, at its next Present.
		if (!m_brains->answers(event.name) || !m_world.registry.valid(entity))
			return;
		if (m_world.role() != ecs::Role::Client)
		{
			if (m_brains->cues == nullptr)
				return;
			if (auto* cues = static_cast<Cues*>(m_brains->cues->get(*m_brains->cues, m_world.registry, entity)))
				cues->push({.name  = static_cast<u32>(event.name),
							.tick  = m_now,
							.by	   = netid_of(*this, source),
							.value = value});
		}
		else if (m_world.registry.all_of<net::Owned>(entity))
		{
			m_brains->own_cues.push_back(event);
		}
	}

	u32 Host::panel_count() const noexcept { return static_cast<u32>(m_brains->panels.size()); }

	StringView Host::panel_name(u32 index) const noexcept
	{
		return index < m_brains->panels.size() ? StringView(m_brains->panels[index].name) : StringView();
	}

	bool Host::panel_failed(u32 index) const noexcept
	{
		return index < m_brains->panels.size() && m_brains->panels[index].disabled;
	}

	bool Host::run_panel(u32 index) noexcept { return script::run_panel(*this, index); }

	bool Host::in_panel() const noexcept { return m_brains->in_panel; }

	f32 Host::stat(ecs::Entity entity, u64 stat, f32 base) const noexcept { return stat_of(*this, entity, stat, base); }

	void Host::sense(ecs::Entity trigger, Span<const physics::Touch> inside) noexcept
	{
		// Each touch this tick, stamped with the epoch; one that was not inside at the last is an entry.
		Brains& brains	= *m_brains;
		const u32 epoch = brains.sense_epoch;
		for (const physics::Touch& touch : inside)
		{
			if (touch.entity == trigger || touch.entity == ecs::NO_ENTITY)
				continue;
			const u64 key = (static_cast<u64>(entt::to_integral(trigger)) << 32) | entt::to_integral(touch.entity);
			const auto it = brains.inside.find(key);
			const bool was_inside = it != brains.inside.end() && (it->second + 1 == epoch || it->second == epoch);
			brains.inside.insert_or_assign(key, epoch);
			if (!was_inside)
				event(trigger, "entered", touch.entity);
		}
	}

	void Host::sense_done() noexcept
	{
		// What was inside at the last epoch and was not stamped in this one has left; then the epoch moves on.
		Brains& brains	= *m_brains;
		const u32 epoch = brains.sense_epoch;
		for (auto it = brains.inside.begin(); it != brains.inside.end();)
		{
			if (it->second == epoch)
			{
				++it;
				continue;
			}
			const auto trigger = static_cast<ecs::Entity>(static_cast<u32>(it->first >> 32));
			const auto entity  = static_cast<ecs::Entity>(static_cast<u32>(it->first));
			if (m_world.registry.valid(trigger))
				event(trigger, "left", m_world.registry.valid(entity) ? entity : ecs::NO_ENTITY);
			it = brains.inside.erase(it);
		}
		++brains.sense_epoch;
	}

	StringView Host::state_of(ecs::Entity entity, u32 slot) const noexcept
	{
		if (m_brains->stategraph == nullptr || slot >= Stategraph::SLOTS || !m_world.registry.valid(entity))
			return {};
		const auto* sg = static_cast<const Stategraph*>(m_brains->stategraph->find(*m_brains->stategraph, m_world.registry, entity));
		if (sg == nullptr)
			return {};
		const Graph* graph = m_brains->graph(sg->slots[slot].graph);
		if (graph == nullptr)
			return {};
		const i16 index = graph->index_of(sg->slots[slot].state);
		return index < 0 ? StringView() : StringView(graph->states[static_cast<size_t>(index)].name);
	}

	void Host::inspect(ecs::Entity entity, Inspection& out) const noexcept
	{
		out		  = {};
		out.tick  = m_now;
		out.found = m_world.registry.valid(entity);
		if (!out.found)
			return;
		Brains& brains = *m_brains;

		if (brains.stategraph != nullptr)
		{
			if (const auto* sg = static_cast<const Stategraph*>(
					brains.stategraph->find(*brains.stategraph, m_world.registry, entity)))
			{
				for (u32 slot = 0; slot < Stategraph::SLOTS; ++slot)
				{
					const Stategraph::Slot& live = sg->slots[slot];
					const Graph* graph			 = brains.graph(live.graph);
					if (graph == nullptr)
						continue;
					Inspection::Slot& made = out.slots.emplace_back();
					made.graph			   = String(graph->name);
					made.since			   = live.since;
					made.elapsed		   = live.state != NO_STATE && m_now >= live.since ? m_now - live.since : 0;
					made.disabled		   = graph->disabled;
					for (const State& state : graph->states)
						made.states.push_back(String(state.name));
					const i16 index = graph->index_of(live.state);
					if (index < 0)
						continue;
					const State& state = graph->states[static_cast<size_t>(index)];
					made.state		   = String(state.name);
					const auto dynamic =
						state.marks_fn >= 0
							? brains.entity_marks.find((static_cast<u64>(entt::to_integral(entity)) << 8) | slot)
							: brains.entity_marks.end();
					const Vector<Mark>& marks =
						dynamic != brains.entity_marks.end() ? dynamic->second.marks : state.marks;
					for (const Mark& mark : marks)
					{
						const auto text = brains.names.find(mark.name);
						made.marks.push_back(
							{.name = text != brains.names.end() ? String(text->second) : String("?"), .at = mark.at});
					}
				}
			}
		}

		Vector<StoryReport> stories(&heap());
		story_report(const_cast<Host&>(*this), entity, stories);
		for (const StoryReport& story : stories)
			out.stories.push_back({.name	= String(story.story),
								   .waiting = String(story.waiting),
								   .event	= String(story.event),
								   .line	= story.line,
								   .wake	= story.wake});

		if (brains.watching == entity)
		{
			const auto name_of = [&](const Transition& step, StateId state) -> String
			{
				if (state == NO_STATE)
					return String("-");
				if (brains.stategraph == nullptr)
					return String("?");
				const auto* sg = static_cast<const Stategraph*>(
					brains.stategraph->find(*brains.stategraph, m_world.registry, entity));
				const Graph* graph = sg != nullptr ? brains.graph(sg->slots[step.slot].graph) : nullptr;
				const i16 index	   = graph != nullptr ? graph->index_of(state) : -1;
				return index >= 0 ? String(graph->states[static_cast<size_t>(index)].name) : String("?");
			};
			for (u32 i = 0; i < brains.history_count; ++i)
			{
				const Transition& step =
					brains
						.history[(brains.history_next + Brains::HISTORY - brains.history_count + i) % Brains::HISTORY];
				out.history.push_back({.slot = step.slot,
									   .from = name_of(step, step.from),
									   .to	 = name_of(step, step.to),
									   .tick = step.tick});
			}
		}

		inspect_modifiers(const_cast<Host&>(*this), entity, out);
	}

	void Host::watch(ecs::Entity entity) noexcept
	{
		if (m_brains->watching == entity)
			return;
		m_brains->watching		= entity;
		m_brains->history_next	= 0;
		m_brains->history_count = 0;
	}

	bool Host::force_state(ecs::Entity entity, u32 slot, StringView state) noexcept
	{
		return script::force_state(*this, entity, slot, state);
	}

	void Host::stories_of(ecs::Entity entity, Vector<StoryReport>& out) const noexcept
	{
		story_report(const_cast<Host&>(*this), entity, out);
	}

	Span<const ecs::PrefabId> Host::retuned() const noexcept
	{
		return {m_brains->retuned.data(), m_brains->retuned.size()};
	}

	void Host::report(StringView path, u32 line, Severity severity, StringView message) noexcept
	{
		for (Problem& known : m_problems)
		{
			if (known.path == path && known.line == line && known.message == message)
			{
				++known.count;
				return;
			}
		}

		if (severity == Severity::Error)
			EMBER_ERROR("script {}:{}: {}", path, line, message);
		else
			EMBER_WARN("script {}:{}: {}", path, line, message);

		if (m_problems.size() >= MAX_PROBLEMS)
			return;

		Problem problem;
		problem.path	 = String(path, &heap());
		problem.line	 = line;
		problem.severity = severity;
		problem.message	 = String(message, &heap());
		m_problems.push_back(std::move(problem));
	}

	void Host::report_lua(StringView path, const char* message) noexcept
	{
		StringView where, rest;
		u32 line;
		split_location(path, message != nullptr ? StringView(message) : StringView("error"), where, line, rest);
		report(where, line, Severity::Error, rest);
	}

	Stats Host::stats() const noexcept
	{
		Stats stats	   = m_stats;
		stats.modules  = 0;
		stats.systems  = static_cast<u32>(m_systems.size());
		stats.disabled = 0;
		for (const Module& module : m_modules)
			stats.modules += module.loaded ? 1 : 0;
		for (const System& system : m_systems)
			stats.disabled += system.disabled ? 1 : 0;
		stats.problems	  = static_cast<u32>(m_problems.size());
		stats.stategraphs = static_cast<u32>(m_brains->graphs.size());
		stats.stories	  = static_cast<u32>(m_brains->stories.size());
		stats.threads	  = static_cast<u32>(m_brains->threads.size());
		stats.lua_bytes	  = m_lua_bytes;
		stats.hash		= m_hash;

		stats.simulate_us = 0.0;
		stats.present_us  = 0.0;
		for (size_t i = 0; i < m_bucket_us.size(); ++i)
		{
			if (i + 1 == m_bucket_us.size())
				stats.present_us = m_bucket_us[i];
			else
				stats.simulate_us += m_bucket_us[i];
		}
		return stats;
	}

	bool Host::write_definitions(StringView path) const noexcept
	{
		String text(&heap());
		write_definitions_text(m_binding, text);

		const auto written = fs::write_file_atomic(
			path, Span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), fs::WriteDurability::None);
		if (!written)
		{
			EMBER_WARN("scripts: cannot write {} ({})", path, enum_name(written.error().code));
			return false;
		}
		return true;
	}

	void run_present(Host& host, ecs::Commands& commands) { host.present(commands); }
}
