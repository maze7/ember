#include "internal.h"

#include <ember/core/hash.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>

#include <algorithm>
#include <cmath>

/**
 * Modifiers: what lasts on an entity for a while and changes it: burning, slowed, a blessing, a badge. A
 * `modifier "x" { lasts, stacking, max_stacks, power, every, tick, stats, show }` declares one; e:inflict("x")
 * puts it on an entity, into its Modifiers component, where every machine that simulates the entity counts it
 * down, and the server's word reaches the rest. Its stats change numbers the game reads through Host::stat()
 * and scripts through e:stat(): the base, then each modifier's adds, then its muls, each per stack, in the
 * order of the modifiers' names. Its tick runs every so many ticks, where its module's directory says. Its
 * show is a client's: entered as the modifier appears, left as it goes, every frame between, and its cues.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		constexpr StringView MODIFIER_KEYS[] = {"lasts", "stacking", "max_stacks", "power", "every",
												"stage", "tick",	 "stats",	   "show"};

		[[nodiscard]] Modifiers* modifiers_of(Host& host, ecs::Entity entity) noexcept
		{
			return host.world().registry.valid(entity) ? host.world().registry.try_get<Modifiers>(entity) : nullptr;
		}

		[[nodiscard]] const Modifiers* modifiers_of(const Host& host, ecs::Entity entity) noexcept
		{
			return host.world().registry.valid(entity) ? host.world().registry.try_get<Modifiers>(entity) : nullptr;
		}

		[[nodiscard]] i32 index_of(const Modifiers& modifiers, u32 name) noexcept
		{
			for (u32 i = 0; i < modifiers.count; ++i)
				if (modifiers.list[i].name == name)
					return static_cast<i32>(i);
			return -1;
		}

		void remove_at(Modifiers& modifiers, u32 index) noexcept
		{
			for (u32 i = index; i + 1 < modifiers.count; ++i)
				modifiers.list[i] = modifiers.list[i + 1];
			modifiers.list[--modifiers.count] = {};
		}

		/** Sorted by name, so every machine combines stats in one order. */
		void sort(Modifiers& modifiers) noexcept
		{
			std::sort(modifiers.list.begin(), modifiers.list.begin() + modifiers.count,
					  [](const Modifier& a, const Modifier& b) { return a.name < b.name; });
		}

		[[nodiscard]] u32 left_of(const Modifier& modifier, u32 now) noexcept
		{
			return modifier.until == 0 ? 0 : (modifier.until > now ? modifier.until - now : 0);
		}

		/** A modifier as its tick and e:modifier() get it: { name, stacks, power, left }. */
		void push_state(lua_State* L, const Modifier& modifier, StringView name, u32 now)
		{
			lua_createtable(L, 0, 4);
			lua_pushlstring(L, name.data(), name.size());
			lua_setfield(L, -2, "name");
			lua_pushnumber(L, static_cast<f64>(modifier.stacks));
			lua_setfield(L, -2, "stacks");
			lua_pushnumber(L, static_cast<f64>(modifier.power));
			lua_setfield(L, -2, "power");
			lua_pushnumber(L, static_cast<f64>(left_of(modifier, now)));
			lua_setfield(L, -2, "left");
		}

		[[nodiscard]] u16 read_u16(lua_State* L, int index, const char* modifier, const char* key, f64 low)
		{
			const f64 value = declared_number(L, index, key);
			if (std::floor(value) != value || value < low || value > 65535.0)
				luaL_error(L, "modifier %s: %s takes a whole number from %g to 65535, not %g", modifier, key, low,
						   value);
			return static_cast<u16>(value);
		}

		[[nodiscard]] u32 read_ticks(lua_State* L, int index, const char* modifier, const char* key)
		{
			const f64 value = declared_number(L, index, key);
			if (std::floor(value) != value || value < 0.0 || value > 4294967295.0)
				luaL_error(L, "modifier %s: %s takes a whole number of ticks, not %g", modifier, key, value);
			return static_cast<u32>(value);
		}

		/** stats = { move_speed = { add = 0, mul = 0.8 }, ... }. */
		void read_stats(lua_State* L, int index, const char* modifier, Vector<StatRow>& out)
		{
			if (!lua_istable(L, index))
				luaL_error(L, "modifier %s: stats is { stat = { add = n, mul = n }, ... }", modifier);
			lua_pushnil(L);
			while (lua_next(L, index) != 0)
			{
				if (!lua_isstring(L, -2) || !lua_istable(L, -1))
					luaL_error(L, "modifier %s: stats is { stat = { add = n, mul = n }, ... }", modifier);
				StatRow row;
				row.stat		= intern_name(HostAccess::of(L), lua_tostring(L, -2));
				const int value = lua_gettop(L);
				lua_pushnil(L);
				while (lua_next(L, value) != 0)
				{
					const StringView key = lua_isstring(L, -2) ? StringView(lua_tostring(L, -2)) : StringView();
					if (key == "add")
						row.add = static_cast<f32>(declared_number(L, lua_gettop(L), "add"));
					else if (key == "mul")
						row.mul = static_cast<f32>(declared_number(L, lua_gettop(L), "mul"));
					else
						luaL_error(L, "modifier %s, stat %s: a stat row has add and mul, not '%s'", modifier,
								   lua_tostring(L, value - 1), key.data());
					lua_pop(L, 1);
				}
				out.push_back(row);
				lua_pop(L, 1);
			}
			std::sort(out.begin(), out.end(), [](const StatRow& a, const StatRow& b) { return a.stat < b.stat; });
		}

		int modifier_body(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			const char* name = lua_tostring(L, lua_upvalueindex(1));
			if (!HostAccess::loading(host))
				luaL_error(L, "modifier is declared at a module's top level, as it loads");
			luaL_checktype(L, 1, LUA_TTABLE);

			const Context context = HostAccess::loading_context(host);
			if (context != Context::Sim && context != Context::Server)
				luaL_error(
					L, "modifier %s: a modifier changes the game, so it is declared in scripts/sim or scripts/server",
					name);
			if (HostAccess::def(host).schema != nullptr)
				return 0;

			ModifierDecl decl;
			decl.name	 = String(name, &heap());
			decl.id		 = static_cast<u32>(intern_name(host, name));
			decl.module	 = static_cast<u32>(HostAccess::loading_module(host));
			decl.context = context;
			decl.stage	 = HostAccess::brains(host).first_stage;

			lua_pushnil(L);
			while (lua_next(L, 1) != 0)
			{
				if (!lua_isstring(L, -2))
					luaL_error(L, "modifier %s: keys are names", name);
				const StringView key = lua_tostring(L, -2);
				const int value		 = lua_gettop(L);

				if (key == "lasts")
					decl.lasts = read_ticks(L, value, name, "lasts");
				else if (key == "every")
					decl.every = read_ticks(L, value, name, "every");
				else if (key == "max_stacks")
					decl.max_stacks = read_u16(L, value, name, "max_stacks", 1.0);
				else if (key == "power")
					decl.power = read_u16(L, value, name, "power", 0.0);
				else if (key == "stacking")
				{
					const StringView how = lua_isstring(L, value) ? StringView(lua_tostring(L, value)) : StringView();
					if (how == "refresh")
						decl.stacking = Stacking::Refresh;
					else if (how == "add")
						decl.stacking = Stacking::Add;
					else if (how == "keep")
						decl.stacking = Stacking::Keep;
					else
						luaL_error(L, "modifier %s: stacking is \"refresh\", \"add\" or \"keep\"", name);
				}
				else if (key == "stage")
				{
					const StageName* stage =
						lua_isstring(L, value) ? HostAccess::binding(host).find_stage(lua_tostring(L, value)) : nullptr;
					if (stage == nullptr)
						luaL_error(L, "modifier %s: no stage named '%s'", name, lua_tostring(L, value));
					decl.stage = stage->index;
				}
				else if (key == "tick")
				{
					if (!lua_isfunction(L, value))
						luaL_error(L, "modifier %s: tick takes a function(e, m)", name);
					decl.tick = lua_ref(L, value);
				}
				else if (key == "stats")
					read_stats(L, value, name, decl.stats);
				else if (key == "show")
				{
					String what("modifier ", &heap());
					what += name;
					read_show(L, value, what.c_str(), decl.show);
				}
				else
				{
					String keys(&heap());
					for (const StringView known : MODIFIER_KEYS)
					{
						if (!keys.empty())
							keys += ", ";
						keys += known;
					}
					luaL_error(L, "modifier %s: unknown key '%s'; a modifier has %s", name, key.data(), keys.c_str());
				}
				lua_pop(L, 1);
			}

			if (decl.tick >= 0 && decl.every == 0)
				luaL_error(L, "modifier %s: tick without every; every says how many ticks apart it runs", name);
			HostAccess::brains(host).modifiers_staging.push_back(std::move(decl));
			return 0;
		}

		int lua_modifier(lua_State* L)
		{
			luaL_checkstring(L, 1);
			lua_pushvalue(L, 1);
			lua_pushcclosure(L, modifier_body, "modifier", 1);
			return 1;
		}

		/** A tick's call: the decl's function on its module's thread, with its rights. False on an error. */
		bool call_tick(Host& host, ModifierDecl& decl, ecs::Entity entity, const Modifier& modifier, u32 now)
		{
			lua_State* thread = HostAccess::thread_of(host, decl.module);
			lua_settop(thread, 0);
			lua_getref(thread, decl.tick);
			push_entity(thread, entity);
			push_state(thread, modifier, decl.name, now);
			HostAccess::begin_call(host, decl.context);
			const int status = lua_pcall(thread, 2, 0, 0);
			HostAccess::end_call(host);
			++HostAccess::stats(host).calls;
			if (status == LUA_OK)
				return true;

			String message("modifier ", &heap());
			message += decl.name;
			message += ": ";
			message += lua_tostring(thread, -1) != nullptr ? lua_tostring(thread, -1) : "error";
			HostAccess::report_lua(host, HostAccess::path_of(host, decl.module), message.c_str());
			lua_settop(thread, 0);
			decl.disabled = true;
			++HostAccess::stats(host).errors;
			return false;
		}

		/** The modifier a verb names, by its declaration; raises for one no script declares. */
		[[nodiscard]] const ModifierDecl& check_decl(lua_State* L, Host& host, int index, const char* verb)
		{
			const char* name		  = luaL_checkstring(L, index);
			const ModifierDecl* found = HostAccess::brains(host).modifier(static_cast<u32>(hash_text(name)));
			if (found == nullptr)
				luaL_error(L, "%s: no modifier named '%s'", verb, name);
			return *found;
		}

		/** The rights to change an entity's modifiers: a sim script on what it predicts, or the server's. */
		void check_may_change(lua_State* L, Host& host, ecs::Entity entity, const char* verb)
		{
			check_context(L, host, ContextMask::Sim | ContextMask::Server, verb);
			check_sim_target(L, host, entity);
			if (HostAccess::brains(host).modifiers_info == nullptr)
				luaL_error(L, "%s: the game registered no Modifiers component (script::register_components)", verb);
		}
	}

	void release_modifier(lua_State* L, ModifierDecl& decl) noexcept
	{
		if (decl.tick >= 0)
			lua_unref(L, decl.tick);
		decl.tick = -1;
		release_show(L, decl.show);
	}

	void run_modifiers(Host& host, u8 stage) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		ecs::World& world	 = host.world();
		if (brains.modifiers_info == nullptr)
			return;
		const entt::sparse_set* storage = world.registry.storage(brains.modifiers_info->type);
		if (storage == nullptr || storage->empty())
			return;

		const u32 now = HostAccess::now(host);
		Vector<ecs::Entity> carriers(&heap());
		for (u32 i = static_cast<u32>(storage->size()); i-- > 0;)
			if (world.registry.all_of<ecs::Simulated>((*storage)[i]))
				carriers.push_back((*storage)[i]);

		for (const ecs::Entity entity : carriers)
		{
			// Counted down once a tick, at the first stage: what has run out goes before anything reads it.
			if (stage == brains.first_stage)
			{
				Modifiers* modifiers = modifiers_of(host, entity);
				for (u32 i = modifiers != nullptr ? modifiers->count : 0; i-- > 0;)
					if (modifiers->list[i].until != 0 && now >= modifiers->list[i].until)
						remove_at(*modifiers, i);
			}

			// Each tick of this stage's, as a copy: a tick may inflict or cure, and so move the rest about.
			const Modifiers* now_on = modifiers_of(host, entity);
			if (now_on == nullptr)
				continue;
			const Modifiers held = *now_on;
			for (u32 i = 0; i < held.count; ++i)
			{
				const Modifier& modifier = held.list[i];
				ModifierDecl* decl		 = nullptr;
				for (ModifierDecl& candidate : brains.modifiers)
					if (candidate.id == modifier.name)
						decl = &candidate;
				if (decl == nullptr || decl->disabled || decl->tick < 0 || decl->stage != stage ||
					!runs_in(decl->context, world.role()))
					continue;

				// Every so many ticks: from its end when it ends, from tick 0 when it lasts.
				const u32 phase = modifier.until != 0 ? modifier.until - now : now;
				if (phase % decl->every != 0)
					continue;
				if (!world.registry.valid(entity))
					break;
				(void)call_tick(host, *decl, entity, modifier, now);
			}
		}
	}

	f32 stat_of(const Host& host, ecs::Entity entity, u64 stat, f32 base) noexcept
	{
		const Modifiers* modifiers = modifiers_of(host, entity);
		if (modifiers == nullptr || modifiers->count == 0)
			return base;

		const Host::Brains& brains = HostAccess::brains(const_cast<Host&>(host));
		f32 value				   = base;
		for (u32 i = 0; i < modifiers->count; ++i)
			if (const ModifierDecl* decl = brains.modifier(modifiers->list[i].name))
				for (const StatRow& row : decl->stats)
					if (row.stat == stat)
						value += row.add * static_cast<f32>(modifiers->list[i].stacks);
		for (u32 i = 0; i < modifiers->count; ++i)
			if (const ModifierDecl* decl = brains.modifier(modifiers->list[i].name))
				for (const StatRow& row : decl->stats)
					if (row.stat == stat)
						for (u32 s = 0; s < modifiers->list[i].stacks; ++s)
							value *= row.mul;
		return value;
	}

	int entity_inflict(lua_State* L, Host& host, ecs::Entity entity)
	{
		// e:inflict("burning", { ticks?, stacks?, power? })
		check_may_change(L, host, entity, "e:inflict");
		const ModifierDecl& decl = check_decl(L, host, 2, "e:inflict");
		u32 lasts				 = decl.lasts;
		u16 stacks				 = 1;
		u16 power				 = decl.power;
		if (lua_istable(L, 3))
		{
			lua_pushnil(L);
			while (lua_next(L, 3) != 0)
			{
				const StringView key = lua_isstring(L, -2) ? StringView(lua_tostring(L, -2)) : StringView();
				if (key == "ticks")
					lasts = read_ticks(L, lua_gettop(L), decl.name.c_str(), "ticks");
				else if (key == "stacks")
					stacks = read_u16(L, lua_gettop(L), decl.name.c_str(), "stacks", 1.0);
				else if (key == "power")
					power = read_u16(L, lua_gettop(L), decl.name.c_str(), "power", 0.0);
				else
					luaL_error(L, "e:inflict: unknown key '%s'; it takes ticks, stacks and power", key.data());
				lua_pop(L, 1);
			}
		}
		else if (!lua_isnoneornil(L, 3))
		{
			luaL_typeerror(L, 3, "{ ticks, stacks, power }");
		}

		Modifiers& modifiers = host.world().registry.get_or_emplace<Modifiers>(entity);

		const u32 now	= HostAccess::now(host);
		const u32 until = lasts == 0 ? 0 : now + lasts;
		if (const i32 at = index_of(modifiers, decl.id); at >= 0)
		{
			Modifier& known = modifiers.list[static_cast<u32>(at)];
			switch (decl.stacking)
			{
				case Stacking::Keep:
					break;
				case Stacking::Add:
					known.stacks = static_cast<u16>(std::min<u32>(known.stacks + stacks, decl.max_stacks));
					known.until	 = until;
					known.power	 = std::max(known.power, power);
					break;
				case Stacking::Refresh:
					known.until = until;
					known.power = std::max(known.power, power);
					break;
			}
		}
		else
		{
			// Full: the one that ends soonest gives way, and the host says so once.
			if (modifiers.count == Modifiers::CAPACITY)
			{
				u32 soonest = 0;
				for (u32 i = 1; i < modifiers.count; ++i)
					if (modifiers.list[i].until != 0 &&
						(modifiers.list[soonest].until == 0 || modifiers.list[i].until < modifiers.list[soonest].until))
						soonest = i;
				HostAccess::report(host, HostAccess::path_of(host, decl.module), 0, Severity::Warning,
								   "an entity holds " + String(std::to_string(Modifiers::CAPACITY).c_str()) +
									   " modifiers at most: one gave way to " + decl.name);
				remove_at(modifiers, soonest);
			}
			modifiers.list[modifiers.count++] = {
				.name	= decl.id,
				.until	= until,
				.stacks = static_cast<u16>(std::min<u32>(stacks, decl.max_stacks)),
				.power	= power,
			};
			sort(modifiers);
		}
		++HostAccess::stats(host).writes;
		return 0;
	}

	int entity_cure(lua_State* L, Host& host, ecs::Entity entity)
	{
		// e:cure("burning"), or e:cure() for every one.
		check_may_change(L, host, entity, "e:cure");
		Modifiers* modifiers = modifiers_of(host, entity);
		if (modifiers == nullptr)
			return 0;
		if (lua_isnoneornil(L, 2))
		{
			*modifiers = {};
			return 0;
		}
		const ModifierDecl& decl = check_decl(L, host, 2, "e:cure");
		if (const i32 at = index_of(*modifiers, decl.id); at >= 0)
			remove_at(*modifiers, static_cast<u32>(at));
		++HostAccess::stats(host).writes;
		return 0;
	}

	int entity_modifier(lua_State* L, Host& host, ecs::Entity entity)
	{
		// e:modifier("burning"): { name, stacks, power, left }, or nil when it has none.
		const ModifierDecl& decl   = check_decl(L, host, 2, "e:modifier");
		const Modifiers* modifiers = modifiers_of(host, entity);
		const i32 at			   = modifiers != nullptr ? index_of(*modifiers, decl.id) : -1;
		if (at < 0)
			lua_pushnil(L);
		else
			push_state(L, modifiers->list[static_cast<u32>(at)], decl.name, HostAccess::now(host));
		return 1;
	}

	int entity_stat(lua_State* L, Host& host, ecs::Entity entity)
	{
		// e:stat("move_speed", base?): base, as the entity's modifiers change it.
		const u64 stat = hash_text(luaL_checkstring(L, 2));
		const f32 base = static_cast<f32>(luaL_optnumber(L, 3, 1.0));
		lua_pushnumber(L, static_cast<f64>(stat_of(host, entity, stat, base)));
		return 1;
	}

	void inspect_modifiers(Host& host, ecs::Entity entity, Inspection& out) noexcept
	{
		const Modifiers* modifiers = modifiers_of(host, entity);
		if (modifiers == nullptr)
			return;
		const Host::Brains& brains = HostAccess::brains(host);
		const u32 now			   = HostAccess::now(host);
		for (u32 i = 0; i < modifiers->count; ++i)
		{
			const Modifier& modifier = modifiers->list[i];
			const ModifierDecl* decl = brains.modifier(modifier.name);
			out.modifiers.push_back({.name	 = decl != nullptr ? String(decl->name) : String("?"),
									 .stacks = modifier.stacks,
									 .power	 = static_cast<f32>(modifier.power),
									 .left	 = left_of(modifier, now)});
		}
	}

	void install_modifiers(lua_State* L, Host& host)
	{
		(void)host;
		lua_pushcfunction(L, lua_modifier, "modifier");
		lua_setglobal(L, "modifier");
	}
}
