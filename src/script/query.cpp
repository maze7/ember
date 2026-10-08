#include "internal.h"

#include <new>

/**
 * `world`: the queries that drive a system's loop, and spawning. A query is one userdata that walks the
 * smallest of the storages it names, from the back as the C++ loops do, and hands out handles: nothing
 * is allocated per entity, and a script's loop costs what a C++ view's does plus the calls.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		/** without(...): the component types a query leaves out. */
		struct Filter
		{
			u8 count = 0;
			ecs::ComponentId ids[MAX_QUERY];
		};

		/** A query under iteration: what it walks, what it checks, and where it is. */
		struct Query
		{
			const entt::sparse_set* lead = nullptr; // the smallest required storage: what is walked
			const entt::sparse_set* required[MAX_QUERY + 1]; // the rest, Simulated among them
			const entt::sparse_set* excluded[MAX_QUERY];
			ecs::ComponentId ids[MAX_QUERY]; // in argument order: the loop's variables
			u8 id_count		  = 0;
			u8 required_count = 0;
			u8 excluded_count = 0;
			u32 remaining	  = 0; // entities of the lead still to look at, from the back
		};

		[[nodiscard]] const entt::sparse_set* storage_of(ecs::World& world, ecs::ComponentId id) noexcept
		{
			return world.registry.storage(world.components()[id].type);
		}

		/** The next entity that has everything required and nothing excluded; false at the end. */
		[[nodiscard]] bool advance(Query& query, ecs::Entity& out) noexcept
		{
			while (query.remaining > 0)
			{
				const ecs::Entity entity = (*query.lead)[--query.remaining];

				bool keep = true;
				for (u32 i = 0; keep && i < query.required_count; ++i)
					keep = query.required[i]->contains(entity);
				for (u32 i = 0; keep && i < query.excluded_count; ++i)
					keep = !query.excluded[i]->contains(entity);

				if (keep)
				{
					out = entity;
					return true;
				}
			}
			return false;
		}

		/** The generic for's step: query_next(query, last) gives the entity and its components, or nothing. */
		int query_next(lua_State* L)
		{
			Query* query = static_cast<Query*>(lua_touserdatatagged(L, 1, TAG_QUERY));
			if (query == nullptr)
				luaL_typeerror(L, 1, "query");

			ecs::Entity entity;
			if (!advance(*query, entity))
				return 0;

			push_entity(L, entity);
			for (u32 i = 0; i < query->id_count; ++i)
				push_component(L, entity, query->ids[i]);
			return 1 + query->id_count;
		}

		/**
		 * world:query(A, B, ..., without(C, ...)): an iterator over the entities with every type named and
		 * none left out. A sim or server script sees what this world simulates; a client script sees every
		 * entity, as a Present system's View does.
		 */
		int world_query(lua_State* L)
		{
			Host& host		  = HostAccess::of(L);
			ecs::World& world = host.world();
			check_context(L, host, ContextMask::All, "world:query");

			Query* query = new (lua_newuserdatataggedwithmetatable(L, sizeof(Query), TAG_QUERY)) Query{};

			const int top = lua_gettop(L) - 1; // the arguments, before the userdata
			for (int index = 2; index <= top; ++index)
			{
				if (const ecs::ComponentId id = to_component_type(L, index); id != ecs::NO_COMPONENT)
				{
					if (query->id_count == MAX_QUERY)
						luaL_error(L, "world:query takes %u component types at most", MAX_QUERY);
					if (HostAccess::binding(host).exposed(id) == nullptr)
						luaL_error(L, "component %u is not exposed to scripts", id);

					const entt::sparse_set* storage = storage_of(world, id);
					if (storage == nullptr)
						luaL_error(L, "no storage for %s", world.components()[id].name.data());

					query->ids[query->id_count++]			= id;
					query->required[query->required_count++] = storage;
					continue;
				}

				if (const Filter* filter = static_cast<const Filter*>(lua_touserdatatagged(L, index, TAG_FILTER)))
				{
					for (u32 i = 0; i < filter->count; ++i)
					{
						if (query->excluded_count == MAX_QUERY)
							luaL_error(L, "world:query leaves out %u component types at most", MAX_QUERY);
						if (const entt::sparse_set* storage = storage_of(world, filter->ids[i]))
							query->excluded[query->excluded_count++] = storage;
					}
					continue;
				}

				luaL_error(L, "world:query takes component types and without(...), not %s at %d",
						   luaL_typename(L, index), index - 1);
			}

			if (query->id_count == 0)
				luaL_error(L, "world:query names at least one component type");

			// What this world simulates, unless the script presents: a client's Present loop sees it all.
			if (HostAccess::context(host) != Context::Client)
				query->required[query->required_count++] = &world.registry.storage<ecs::Simulated>();

			// The smallest storage leads; the rest are checked for each of its entities.
			u32 smallest = 0;
			for (u32 i = 1; i < query->required_count; ++i)
				if (query->required[i]->size() < query->required[smallest]->size())
					smallest = i;
			query->lead						 = query->required[smallest];
			query->required[smallest]		 = query->required[query->required_count - 1];
			--query->required_count;
			query->remaining = static_cast<u32>(query->lead->size());

			++HostAccess::stats(host).queries;

			// The generic for's triple: the step, the state, and nothing before the first.
			lua_pushcfunction(L, query_next, "query");
			lua_pushvalue(L, -2);
			lua_pushnil(L);
			return 3;
		}

		/** without(A, B, ...): the filter a query takes last. */
		int world_without(lua_State* L)
		{
			const int count = lua_gettop(L);
			if (count < 1)
				luaL_error(L, "without() names at least one component type");
			if (count > static_cast<int>(MAX_QUERY))
				luaL_error(L, "without() takes %u component types at most", MAX_QUERY);

			Filter* filter = static_cast<Filter*>(lua_newuserdatataggedwithmetatable(L, sizeof(Filter), TAG_FILTER));
			filter->count  = 0;
			for (int index = 1; index <= count; ++index)
				filter->ids[filter->count++] = check_component_type(L, index);
			return 1;
		}

		/**
		 * world:spawn("prefab", { Position = { value = vector.create(8, 8, 0) } }): a prefab's entity, now, with the
		 * fields given over the prefab's, handed back so the script can go on with it. The script point runs
		 * alone, so the world is its to add to; a query under way never visits what it adds. The server's:
		 * what it spawns reaches every client through replication, so a predicting client never spawns.
		 */
		int world_spawn(lua_State* L)
		{
			Host& host		  = HostAccess::of(L);
			ecs::World& world = host.world();
			check_context(L, host, ContextMask::Server, "world:spawn");
			(void)check_commands(L, host, "world:spawn"); // inside a point

			const char* name		 = luaL_checkstring(L, 2);
			const ecs::Prefab* prefab = world.prefabs().find(name);
			if (prefab == nullptr)
				luaL_error(L, "no prefab named '%s'", name);

			const ecs::Entity entity = world.create(*prefab, world.role() != ecs::Role::Client);
			++HostAccess::stats(host).writes;

			if (!lua_isnoneornil(L, 3))
			{
				luaL_checktype(L, 3, LUA_TTABLE);
				const bool checks = HostAccess::def(host).checks;

				lua_pushnil(L);
				while (lua_next(L, 3) != 0)
				{
					// { Position = { ... } }: the key is the component's name, as the entity's fields are.
					int atom		= -1;
					const char* key = lua_tostringatom(L, -2, &atom);
					if (key == nullptr)
						luaL_error(L, "world:spawn's overrides are keyed by component name, not %s",
								   luaL_typename(L, -2));
					const ecs::ComponentId id = HostAccess::component_of_atom(host, atom);
					if (id == ecs::NO_COMPONENT)
						luaL_error(L, "world:spawn: '%s' is not a component", key);
					const Exposed* exposed = HostAccess::binding(host).exposed(id);
					if (exposed == nullptr)
						luaL_error(L, "component %s is not exposed to scripts", key);
					if (!lua_istable(L, -1))
						luaL_error(L, "world:spawn: %s takes a table of fields, not %s", key, luaL_typename(L, -1));

					// Over the entity's value, which is the prefab's, or the type's defaults when the prefab lacks it.
					const ecs::ComponentInfo& info = *exposed->info;
					if (!ecs::lives_in(info.kind, world.role()))
					{
						lua_pop(L, 1);
						continue;
					}
					void* bytes = info.get(info, world.registry, entity);
					if (bytes == nullptr)
					{
						info.emplace(info, world.registry, entity, info.defaults.data());
						bytes = info.get(info, world.registry, entity);
					}
					fill_component(L, *exposed, lua_gettop(L), bytes, checks, nullptr);
					lua_pop(L, 1);
				}
			}

			push_entity(L, entity);
			return 1;
		}

		int filter_tostring(lua_State* L)
		{
			lua_pushstring(L, "Filter");
			return 1;
		}

		int query_tostring(lua_State* L)
		{
			lua_pushstring(L, "Query");
			return 1;
		}
	}

	void install_world(lua_State* L, Host& host)
	{
		(void)host;

		// The two userdata types, each with the metatable its tag makes for it.
		lua_newtable(L);
		lua_pushstring(L, "Filter");
		lua_setfield(L, -2, "__type");
		lua_pushcfunction(L, filter_tostring, "__tostring");
		lua_setfield(L, -2, "__tostring");
		lua_setreadonly(L, -1, true);
		lua_setuserdatametatable(L, TAG_FILTER);

		lua_newtable(L);
		lua_pushstring(L, "Query");
		lua_setfield(L, -2, "__type");
		lua_pushcfunction(L, query_tostring, "__tostring");
		lua_setfield(L, -2, "__tostring");
		lua_setreadonly(L, -1, true);
		lua_setuserdatametatable(L, TAG_QUERY);

		lua_newtable(L);
		lua_pushcfunction(L, world_query, "world.query");
		lua_setfield(L, -2, "query");
		lua_pushcfunction(L, world_spawn, "world.spawn");
		lua_setfield(L, -2, "spawn");
		lua_setglobal(L, "world");

		lua_pushcfunction(L, world_without, "without");
		lua_setglobal(L, "without");
	}
}
