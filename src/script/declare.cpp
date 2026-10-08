#include "internal.h"

#include <ember/core/hash.h>
#include <ember/core/logger.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

/**
 * What a module declares at its top level: `component "X" { kind, fields }`, `prefab "x" { ... }` and
 * `stategraph "x" { ... }`, each curried so the name reads first, and the unit declarators the values
 * are written in. In schema mode the declarations are collected for the registry; at run time, when
 * the registry already has them, a component or prefab declaration checks itself against what was
 * registered and reports a difference as "restart to apply", and a stategraph declaration is loaded.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		// --- units ----------------------------------------------------------------------------------

		enum class Unit : u8
		{
			Tiles,	 // a distance: texels in the sim, f32
			Ticks,	 // a duration in ticks, u16
			Seconds, // a duration in seconds: ticks in the sim, u16
			Count,	 // a small whole number, u16
			Tick,	 // a moment: a tick number, u32, 0 for none
			Int,	 // a whole number that may go below zero, i32
			Name,	 // a name's hash, u64: name("idle")
			Entity,	 // another entity, by network id, u32: entity() is none
			Count_
		};

		constexpr const char* UNIT_NAMES[] = {"tiles", "ticks", "seconds", "count", "tick", "int", "name", "entity"};
		static_assert(std::size(UNIT_NAMES) == static_cast<size_t>(Unit::Count_));

		[[nodiscard]] ecs::FieldType type_of(Unit unit) noexcept
		{
			switch (unit)
			{
				case Unit::Tiles:
					return ecs::FieldType::F32;
				case Unit::Ticks:
				case Unit::Seconds:
				case Unit::Count:
					return ecs::FieldType::U16;
				case Unit::Tick:
					return ecs::FieldType::U32;
				case Unit::Int:
					return ecs::FieldType::I32;
				case Unit::Name:
					return ecs::FieldType::Name;
				case Unit::Entity:
					return ecs::FieldType::Entity;
				default:
					return ecs::FieldType::F32;
			}
		}

		/** The sim's number for a value in a unit: texels for tiles, ticks for seconds. */
		[[nodiscard]] f64 convert(lua_State* L, const Binding& binding, Unit unit, f64 x)
		{
			switch (unit)
			{
				case Unit::Tiles:
					if (binding.texels_per_tile() <= 0.0)
						luaL_error(L, "tiles(): the game set no units (Binding::units)");
					return x * binding.texels_per_tile();
				case Unit::Seconds:
					if (binding.ticks_per_second() <= 0.0)
						luaL_error(L, "seconds(): the game set no units (Binding::units)");
					return std::round(x * binding.ticks_per_second());
				case Unit::Ticks:
				case Unit::Count:
				case Unit::Tick:
				case Unit::Int:
					if (std::floor(x) != x)
						luaL_error(L, "%s() takes a whole number, not %g", UNIT_NAMES[static_cast<size_t>(unit)], x);
					return x;
				default:
					return x;
			}
		}

		/** A number, or a unit value's number; raises for anything else. */
		[[nodiscard]] f64 number_or_unit(lua_State* L, int index, const char* what)
		{
			if (lua_istable(L, index))
			{
				lua_rawgetfield(L, index, "n");
				if (lua_isnumber(L, -1))
				{
					const f64 value = lua_tonumber(L, -1);
					lua_pop(L, 1);
					return value;
				}
				lua_pop(L, 1);
			}
			int is_number	= 0;
			const f64 value = lua_tonumberx(L, index, &is_number);
			if (!is_number)
				luaL_error(L, "%s takes a number, not %s", what, luaL_typename(L, index));
			return value;
		}

		/** The unit a schema-mode value was declared in, or Count_ for a plain table. */
		[[nodiscard]] Unit unit_of(lua_State* L, int index) noexcept
		{
			if (!lua_istable(L, index))
				return Unit::Count_;
			lua_rawgetfield(L, index, "__unit");
			const Unit unit = lua_isnumber(L, -1) ? static_cast<Unit>(lua_tointeger(L, -1)) : Unit::Count_;
			lua_pop(L, 1);
			return unit;
		}

		/**
		 * tiles(2.5), ticks(12), ...: the sim's number. In schema mode the number comes wrapped with its
		 * unit, so a component declaration knows the field's type; the wrapper adds, multiplies and
		 * compares as the number would, and loses the unit doing so.
		 */
		int unit_call(lua_State* L)
		{
			Host& host		= HostAccess::of(L);
			const Unit unit = static_cast<Unit>(lua_tointeger(L, lua_upvalueindex(1)));

			// name("idle") and entity(): a hash, and none. Running, a Name handle and nil; in the schema pass,
			// the unit value a component declaration reads its field type and starting value from.
			if (unit == Unit::Name || unit == Unit::Entity)
			{
				const u64 bits = unit == Unit::Name ? check_name(L, 1) : 0;
				if (HostAccess::def(host).schema == nullptr)
				{
					if (unit == Unit::Name)
						push_name(L, bits);
					else
						lua_pushlightuserdatatagged(L, nullptr, TAG_NO_ENTITY); // never nil: a table keeps the key
					return 1;
				}
				lua_createtable(L, 0, 3);
				lua_pushinteger(L, static_cast<int>(unit));
				lua_setfield(L, -2, "__unit");
				lua_pushnumber(L, 0.0);
				lua_setfield(L, -2, "n");
				if (unit == Unit::Name)
				{
					lua_pushvalue(L, 1);
					lua_setfield(L, -2, "s");
				}
				lua_getref(L, static_cast<int>(lua_tointeger(L, lua_upvalueindex(2))));
				lua_setmetatable(L, -2);
				return 1;
			}

			const f64 x		= unit == Unit::Tick ? luaL_optnumber(L, 1, 0.0) : luaL_checknumber(L, 1);
			const f64 value = convert(L, HostAccess::binding(host), unit, x);

			if (HostAccess::def(host).schema == nullptr)
			{
				lua_pushnumber(L, value);
				return 1;
			}

			lua_createtable(L, 0, 2);
			lua_pushinteger(L, static_cast<int>(unit));
			lua_setfield(L, -2, "__unit");
			lua_pushnumber(L, value);
			lua_setfield(L, -2, "n");
			lua_getref(L, static_cast<int>(lua_tointeger(L, lua_upvalueindex(2))));
			lua_setmetatable(L, -2);
			return 1;
		}

		template <class Op> int unit_arith(lua_State* L)
		{
			const f64 a = number_or_unit(L, 1, "arithmetic on a unit value");
			const f64 b = lua_isnoneornil(L, 2) ? 0.0 : number_or_unit(L, 2, "arithmetic on a unit value");
			lua_pushnumber(L, Op{}(a, b));
			return 1;
		}

		struct Add
		{
			f64 operator()(f64 a, f64 b) const noexcept { return a + b; }
		};
		struct Sub
		{
			f64 operator()(f64 a, f64 b) const noexcept { return a - b; }
		};
		struct Mul
		{
			f64 operator()(f64 a, f64 b) const noexcept { return a * b; }
		};
		struct Div
		{
			f64 operator()(f64 a, f64 b) const noexcept { return a / b; }
		};
		struct Neg
		{
			f64 operator()(f64 a, f64) const noexcept { return -a; }
		};

		int unit_lt(lua_State* L)
		{
			lua_pushboolean(L, number_or_unit(L, 1, "<") < number_or_unit(L, 2, "<"));
			return 1;
		}
		int unit_le(lua_State* L)
		{
			lua_pushboolean(L, number_or_unit(L, 1, "<=") <= number_or_unit(L, 2, "<="));
			return 1;
		}
		int unit_tostring(lua_State* L)
		{
			const Unit unit = unit_of(L, 1);
			lua_pushfstring(L, "%s(%g)", unit < Unit::Count_ ? UNIT_NAMES[static_cast<size_t>(unit)] : "unit",
							number_or_unit(L, 1, "tostring"));
			return 1;
		}

		// --- values ---------------------------------------------------------------------------------

		/** Whether the declaration is being made where declarations are made: a module's top level. */
		void check_declaring(lua_State* L, const Host& host, const char* what)
		{
			if (!HostAccess::loading(host))
				luaL_error(L, "%s is declared at a module's top level, as it loads", what);
		}

		[[nodiscard]] bool parse_kind_name(StringView name, ecs::Kind& out) noexcept
		{
			constexpr std::pair<StringView, ecs::Kind> KINDS[] = {
				{"Sim", ecs::Kind::Sim},				 {"Server", ecs::Kind::Server},
				{"Client", ecs::Kind::Client},			 {"Replicated", ecs::Kind::Replicated},
				{"Interpolated", ecs::Kind::Interpolated}, {"Predicted", ecs::Kind::Predicted},
				{"OwnerOnly", ecs::Kind::OwnerOnly},
			};
			for (const auto& [kind_name, kind] : KINDS)
			{
				if (kind_name == name)
				{
					out |= kind;
					return true;
				}
			}
			return false;
		}

		/** kind = "Server", or kind = { "Interpolated", "Predicted" }. */
		[[nodiscard]] ecs::Kind read_kind(lua_State* L, int index, const char* component)
		{
			ecs::Kind kind = ecs::Kind::None;
			if (lua_isstring(L, index))
			{
				if (!parse_kind_name(lua_tostring(L, index), kind))
					luaL_error(L, "component %s: kind '%s' is not one; the kinds are Sim, Server, Client, Replicated, "
								  "Interpolated, Predicted and OwnerOnly",
							   component, lua_tostring(L, index));
				return kind;
			}
			if (!lua_istable(L, index))
				luaL_error(L, "component %s: kind takes a name or a list of names, not %s", component,
						   luaL_typename(L, index));

			const int count = lua_objlen(L, index);
			for (int i = 1; i <= count; ++i)
			{
				lua_rawgeti(L, index, i);
				if (!lua_isstring(L, -1) || !parse_kind_name(lua_tostring(L, -1), kind))
					luaL_error(L, "component %s: kind %d is not a kind name", component, i);
				lua_pop(L, 1);
			}
			return kind;
		}

		/** A declaration's field from its value: a number, a boolean, a vector or a unit value. */
		void read_field_def(lua_State* L, int index, const char* component, ecs::FieldDef& out)
		{
			switch (lua_type(L, index))
			{
				case LUA_TBOOLEAN:
					out.type  = ecs::FieldType::Bool;
					out.value = lua_toboolean(L, index) != 0 ? 1.0 : 0.0;
					return;
				case LUA_TNUMBER:
					out.type  = ecs::FieldType::F32;
					out.value = lua_tonumber(L, index);
					return;
				case LUA_TVECTOR:
				{
					const f32* v = lua_tovector(L, index);
					out.type	 = ecs::FieldType::Vec2;
					out.value	 = v[0];
					out.y		 = v[1];
					return;
				}
				case LUA_TLIGHTUSERDATA:
				{
					// Running, name() and entity() give their handles rather than unit values.
					const int tag = lua_lightuserdatatag(L, index);
					if (tag == TAG_NAME)
					{
						out.type = ecs::FieldType::Name;
						out.bits = check_name(L, index);
						return;
					}
					if (tag == TAG_NO_ENTITY)
					{
						out.type = ecs::FieldType::Entity;
						return;
					}
					break;
				}
				case LUA_TTABLE:
				{
					const Unit unit = unit_of(L, index);
					if (unit == Unit::Count_)
						break;
					out.type = type_of(unit);
					if (unit == Unit::Name)
					{
						lua_rawgetfield(L, index, "s");
						out.bits = lua_isstring(L, -1) ? intern_name(HostAccess::of(L), lua_tostring(L, -1)) : 0;
						lua_pop(L, 1);
						return;
					}
					out.value = number_or_unit(L, index, "a field");
					return;
				}
				default:
					break;
			}
			luaL_error(L,
					   "component %s: field '%s' takes a number, true or false, a vector, a unit value such as "
					   "tiles(2) or ticks(12), name(\"idle\") or entity(), not %s",
					   component, out.name.c_str(), luaL_typename(L, index));
		}

		/** Whether a registered type matches a declaration: the same kind and the same fields at the same values. */
		[[nodiscard]] bool matches(const ecs::ComponentInfo& info, const ecs::DynamicComponentDef& def, String& why)
		{
			if (!info.dynamic)
			{
				why = "a C++ component of that name exists";
				return false;
			}
			if (info.kind != ecs::normalize(def.kind))
			{
				why = "its kind changed";
				return false;
			}
			if (info.fields.size() != def.fields.size())
			{
				why = "its fields changed";
				return false;
			}
			// At run time the unit declarators give plain numbers, so a number matches any numeric type: the
			// types themselves are the schema pass's to check, at start.
			const auto numeric = [](ecs::FieldType type)
			{
				return type != ecs::FieldType::Bool && type != ecs::FieldType::Vec2 && type != ecs::FieldType::Name &&
					   type != ecs::FieldType::Entity;
			};
			for (const ecs::FieldDef& field : def.fields)
			{
				const ecs::FieldInfo* known = info.field(field.name);
				if (known == nullptr || (known->type != field.type && !(numeric(known->type) && numeric(field.type))))
				{
					why = "field '" + field.name + "' changed";
					return false;
				}
				if (known->type == ecs::FieldType::Name || known->type == ecs::FieldType::Entity)
				{
					if (ecs::read_field_bits(*known, info.defaults.data()) != field.bits)
					{
						why = "field '" + field.name + "' starts at another value";
						return false;
					}
					continue;
				}
				const f64 value = ecs::read_field(*known, info.defaults.data());
				const f64 y		= field.type == ecs::FieldType::Vec2
									  ? static_cast<f64>(*reinterpret_cast<const f32*>(info.defaults.data() + known->offset + 4))
									  : 0.0;
				const f64 want	= known->type == ecs::FieldType::F32 || known->type == ecs::FieldType::Vec2
									  ? static_cast<f64>(static_cast<f32>(field.value))
									  : field.value;
				if (value != want || (field.type == ecs::FieldType::Vec2 && y != static_cast<f64>(field.y)))
				{
					why = "field '" + field.name + "' starts at another value";
					return false;
				}
			}
			return true;
		}

		// --- component ------------------------------------------------------------------------------

		int component_body(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			const char* name = lua_tostring(L, lua_upvalueindex(1));
			check_declaring(L, host, "component");
			luaL_checktype(L, 1, LUA_TTABLE);

			ecs::DynamicComponentDef def;
			def.name = String(name, &heap());
			def.kind = ecs::Kind::Sim;

			lua_pushnil(L);
			while (lua_next(L, 1) != 0)
			{
				if (!lua_isstring(L, -2))
					luaL_error(L, "component %s: fields are named, not %s", name, luaL_typename(L, -2));
				const StringView key = lua_tostring(L, -2);

				if (key == "kind")
				{
					def.kind = read_kind(L, lua_gettop(L), name);
				}
				else
				{
					ecs::FieldDef field;
					field.name = String(key, &heap());
					read_field_def(L, lua_gettop(L), name, field);
					def.fields.push_back(std::move(field));
				}
				lua_pop(L, 1);
			}

			if (!ecs::one_home(ecs::normalize(def.kind)))
				luaL_error(L, "component %s: the kind names exactly one of Sim, Server and Client, or net flags, which mean Sim",
						   name);

			// Sorted by name: the layout every machine computes, whatever order the file wrote them in.
			std::sort(def.fields.begin(), def.fields.end(),
					  [](const ecs::FieldDef& a, const ecs::FieldDef& b) { return a.name < b.name; });

			if (Schema* schema = HostAccess::def(host).schema; schema != nullptr)
			{
				for (const ecs::DynamicComponentDef& known : schema->components)
					if (known.name == def.name)
						luaL_error(L, "component %s is declared twice", name);
				if (host.world().components().find(StringView(def.name)) != nullptr)
					luaL_error(L, "component %s: a C++ component has that name", name);
				schema->components.push_back(std::move(def));
				return 0;
			}

			// Running: the registry was made from the schema pass; a change since is for the next start.
			const StringView path		   = HostAccess::loading_path(host);
			const ecs::ComponentInfo* info = host.world().components().find(StringView(def.name));
			String why(&heap());
			if (info == nullptr)
				HostAccess::report(host, path, 0, Severity::Warning,
								   "component " + def.name + " is new: restart to apply it");
			else if (!matches(*info, def, why))
				HostAccess::report(host, path, 0, Severity::Warning,
								   "component " + def.name + " changed (" + why + "): restart to apply it");
			return 0;
		}

		int lua_component(lua_State* L)
		{
			luaL_checkstring(L, 1);
			lua_pushvalue(L, 1);
			lua_pushcclosure(L, component_body, "component", 1);
			return 1;
		}

		// --- prefab ---------------------------------------------------------------------------------

		/** Who converts a component's table into bytes: the registered exposure, or a described one in schema mode. */
		struct Lookup
		{
			Host& host;
			bool schema;

			[[nodiscard]] const Exposed* find(StringView component) const noexcept
			{
				if (schema)
				{
					for (const Exposed& described : HostAccess::brains(host).described_exposed)
						if (described.info->name == component)
							return &described;
				}
				const ecs::ComponentInfo* info = host.world().components().find(component);
				return info != nullptr ? HostAccess::binding(host).exposed(info->id) : nullptr;
			}
		};

		/** { field = value, ... } at `table` into one entry's bytes: the type's defaults, then the fields given. */
		void convert_entry(lua_State* L, Host& host, const Lookup& lookup, const char* prefab, StringView component,
						   int table, PrefabEntry& out)
		{
			out.component		   = String(component, &heap());
			const Exposed* exposed = lookup.find(component);
			if (exposed == nullptr)
				luaL_error(L, "prefab %s: '%.*s' is not a component scripts know", prefab, static_cast<int>(component.size()),
						   component.data());

			const ecs::ComponentInfo& info = *exposed->info;
			out.bytes.assign(info.defaults.begin(), info.defaults.end());
			out.mask.assign(info.size, 0);

			// Stategraph = "name", or { "a", "b" }: each slot's graph by its number; no state is entered yet.
			if (component == "Stategraph" && (lua_isstring(L, table) || lua_istable(L, table)))
			{
				Stategraph graphs;
				if (lua_isstring(L, table))
				{
					graphs.slots[0].graph = graph_id(lua_tostring(L, table));
				}
				else
				{
					const int count = lua_objlen(L, table);
					if (count < 1 || count > static_cast<int>(Stategraph::SLOTS))
						luaL_error(L, "prefab %s: Stategraph names one graph, or a list of up to %u", prefab,
								   Stategraph::SLOTS);
					for (int i = 1; i <= count; ++i)
					{
						lua_rawgeti(L, table, i);
						if (!lua_isstring(L, -1))
							luaL_error(L, "prefab %s: Stategraph %d is not a name", prefab, i);
						graphs.slots[static_cast<size_t>(i - 1)].graph = graph_id(lua_tostring(L, -1));
						lua_pop(L, 1);
					}
				}
				if (out.bytes.size() >= sizeof(graphs))
				{
					std::memcpy(out.bytes.data(), &graphs, sizeof(graphs));
					std::fill(out.mask.begin(), out.mask.begin() + sizeof(graphs), u8{1});
				}
				return;
			}

			if (!lua_istable(L, table))
				luaL_error(L, "prefab %s: %.*s takes a table of fields, or false to take it away, not %s", prefab,
						   static_cast<int>(component.size()), component.data(), luaL_typename(L, table));
			fill_component(L, *exposed, table, out.bytes.data(), HostAccess::def(host).checks, out.mask.data());
		}

		/** A table of { Comp = {...} | false } at `table` into entries. */
		void convert_entries(lua_State* L, Host& host, const Lookup& lookup, const char* prefab, int table,
							 Vector<PrefabEntry>& out)
		{
			lua_pushnil(L);
			while (lua_next(L, table) != 0)
			{
				if (!lua_isstring(L, -2))
					luaL_error(L, "prefab %s: components are named, not %s", prefab, luaL_typename(L, -2));
				const StringView component = lua_tostring(L, -2);

				PrefabEntry entry;
				if (lua_isboolean(L, -1) && lua_toboolean(L, -1) == 0)
				{
					entry.component = String(component, &heap());
					entry.removed	= true;
					if (lookup.find(component) == nullptr)
						luaL_error(L, "prefab %s: '%.*s' is not a component scripts know", prefab,
								   static_cast<int>(component.size()), component.data());
				}
				else
				{
					convert_entry(L, host, lookup, prefab, component, lua_gettop(L), entry);
				}
				out.push_back(std::move(entry));
				lua_pop(L, 1);
			}
		}

		[[nodiscard]] Vector<String> read_names(lua_State* L, int table, const char* prefab, const char* what)
		{
			Vector<String> names(&heap());
			if (!lua_istable(L, table))
				luaL_error(L, "prefab %s: %s takes a list of group names, not %s", prefab, what, luaL_typename(L, table));
			const int count = lua_objlen(L, table);
			for (int i = 1; i <= count; ++i)
			{
				lua_rawgeti(L, table, i);
				if (!lua_isstring(L, -1))
					luaL_error(L, "prefab %s: %s names groups, not %s", prefab, what, luaL_typename(L, -1));
				names.push_back(String(lua_tostring(L, -1), &heap()));
				lua_pop(L, 1);
			}
			return names;
		}

		/** The whole `prefab "x" { ... }` table at `table` into a declaration, values converted. */
		void convert_prefab(lua_State* L, Host& host, const Lookup& lookup, const char* name, int table, PrefabDecl& out)
		{
			out.name = String(name, &heap());

			lua_pushnil(L);
			while (lua_next(L, table) != 0)
			{
				if (!lua_isstring(L, -2))
					luaL_error(L, "prefab %s: keys are names, not %s", name, luaL_typename(L, -2));
				const StringView key = lua_tostring(L, -2);
				const int value		 = lua_gettop(L);

				if (key == "extends")
				{
					if (!lua_isstring(L, value))
						luaL_error(L, "prefab %s: extends names a prefab", name);
					out.extends = String(lua_tostring(L, value), &heap());
				}
				else if (key == "groups")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "prefab %s: groups is a table of named groups", name);
					lua_pushnil(L);
					while (lua_next(L, value) != 0)
					{
						if (!lua_isstring(L, -2) || !lua_istable(L, -1))
							luaL_error(L, "prefab %s: a group is a name and a table of components", name);
						PrefabGroup group;
						group.name = String(lua_tostring(L, -2), &heap());
						convert_entries(L, host, lookup, name, lua_gettop(L), group.entries);
						out.groups.push_back(std::move(group));
						lua_pop(L, 1);
					}
				}
				else if (key == "start")
				{
					out.start = read_names(L, value, name, "start");
				}
				else if (key == "events")
				{
					if (!lua_istable(L, value))
						luaL_error(L, "prefab %s: events is a table of named events", name);
					lua_pushnil(L);
					while (lua_next(L, value) != 0)
					{
						if (!lua_isstring(L, -2) || !lua_istable(L, -1))
							luaL_error(L, "prefab %s: an event is a name and { add = {...}, remove = {...} }", name);
						PrefabEvent event;
						event.name		= String(lua_tostring(L, -2), &heap());
						const int rules = lua_gettop(L);
						lua_pushnil(L);
						while (lua_next(L, rules) != 0)
						{
							const StringView rule = lua_isstring(L, -2) ? StringView(lua_tostring(L, -2)) : StringView();
							if (rule == "add")
								event.add = read_names(L, lua_gettop(L), name, "add");
							else if (rule == "remove")
								event.remove = read_names(L, lua_gettop(L), name, "remove");
							else
								luaL_error(L, "prefab %s, event %s: an event has add and remove", name, event.name.c_str());
							lua_pop(L, 1);
						}
						out.events.push_back(std::move(event));
						lua_pop(L, 1);
					}
				}
				else
				{
					PrefabEntry entry;
					if (lua_isboolean(L, value) && lua_toboolean(L, value) == 0)
					{
						entry.component = String(key, &heap());
						entry.removed	= true;
						if (lookup.find(key) == nullptr)
							luaL_error(L, "prefab %s: '%s' is not a component scripts know", name, key.data());
					}
					else
					{
						convert_entry(L, host, lookup, name, key, value, entry);
					}
					out.entries.push_back(std::move(entry));
				}
				lua_pop(L, 1);
			}

			// Every group the start and the events name exists.
			const auto has_group = [&](const String& group)
			{
				for (const PrefabGroup& candidate : out.groups)
					if (candidate.name == group)
						return true;
				return false;
			};
			for (const String& group : out.start)
				if (!has_group(group))
					luaL_error(L, "prefab %s: start names group '%s', which it has not", name, group.c_str());
			for (const PrefabEvent& event : out.events)
			{
				for (const String& group : event.add)
					if (!has_group(group))
						luaL_error(L, "prefab %s, event %s: adds group '%s', which it has not", name, event.name.c_str(),
								   group.c_str());
				for (const String& group : event.remove)
					if (!has_group(group))
						luaL_error(L, "prefab %s, event %s: removes group '%s', which it has not", name,
								   event.name.c_str(), group.c_str());
			}
		}

		enum class PrefabDiff : u8
		{
			Same,	// what this world makes already
			Values, // the same components at other values: tuned live
			Shape,	// another set of components, or one that cannot be composed: for the next start
		};

		/** What a declaration composes now against what this world makes the prefab from. */
		[[nodiscard]] PrefabDiff prefab_diff(const ecs::World& world, const ecs::Prefab& current,
											 const PrefabDecl& decl, Vector<ecs::PrefabComponent>& composed)
		{
			const ecs::Prefab* base = decl.extends.empty() ? nullptr : world.prefabs().find(StringView(decl.extends));
			if (!decl.extends.empty() && base == nullptr)
				return PrefabDiff::Shape;

			String why(&heap());
			if (!compose_prefab(world.components(), base, decl, true, composed, why))
				return PrefabDiff::Shape;
			if (composed.size() != current.components.size())
				return PrefabDiff::Shape;

			PrefabDiff diff = PrefabDiff::Same;
			for (size_t i = 0; i < composed.size(); ++i)
			{
				const ecs::PrefabComponent& a = composed[i];
				const ecs::PrefabComponent& b = current.components[i];
				if (a.id != b.id || a.value.size() != b.value.size())
					return PrefabDiff::Shape;
				if (std::memcmp(a.value.data(), b.value.data(), a.value.size()) != 0)
					diff = PrefabDiff::Values;
			}
			return diff;
		}

		/**
		 * The prefab's new values onto this world, and onto every entity made from it that still holds the old
		 * ones: field by field for a component scripts see, so a field the game has moved since keeps its value
		 * and the rest follow the file; whole for the others.
		 */
		void retune(Host& host, const ecs::Prefab& registered, Vector<ecs::PrefabComponent> composed)
		{
			ecs::World& world		 = host.world();
			const ecs::Prefab before = world.prefab_of(registered.id); // a copy: the old values
			world.retune_prefab(registered.id, std::move(composed));
			const ecs::Prefab& after = world.prefab_of(registered.id);

			u32 entities = 0;
			for (const auto [entity, ref] : world.registry.view<const ecs::PrefabRef>().each())
			{
				if (ref.id != registered.id)
					continue;
				++entities;
				for (const ecs::PrefabComponent& fresh : after.components)
				{
					const ecs::PrefabComponent* old = before.find(fresh.id);
					if (old == nullptr || old->value == fresh.value)
						continue;
					const ecs::ComponentInfo& info = world.components()[fresh.id];
					u8* bytes					   = static_cast<u8*>(info.get(info, world.registry, entity));
					if (bytes == nullptr)
						continue;

					const Exposed* exposed = HostAccess::binding(host).exposed(fresh.id);
					if (exposed != nullptr && !exposed->fields.empty())
					{
						for (const Field& field : exposed->fields)
							if (std::memcmp(bytes + field.offset, old->value.data() + field.offset, field.size) == 0)
								std::memcpy(bytes + field.offset, fresh.value.data() + field.offset, field.size);
					}
					else if (std::memcmp(bytes, old->value.data(), info.size) == 0)
					{
						std::memcpy(bytes, fresh.value.data(), info.size);
					}
				}
			}

			HostAccess::brains(host).retuned.push_back(registered.id);
			EMBER_INFO("script prefab {} retuned live: {} entit{} follow", StringView(registered.name), entities,
					   entities == 1 ? "y" : "ies");
		}

		/** The groups and events of a prefab the running host keeps, for e:event(). */
		void record_extras(Host& host, const ecs::Prefab& prefab, const PrefabDecl& decl)
		{
			Host::Brains& brains = HostAccess::brains(host);

			Extras extras;
			extras.prefab = prefab.id;
			extras.module = static_cast<u32>(HostAccess::loading_module(host));

			// The prefab without any group: what a removed group's component goes back to.
			const ecs::Prefab* base = decl.extends.empty() ? nullptr : host.world().prefabs().find(StringView(decl.extends));
			String why(&heap());
			(void)compose_prefab(host.world().components(), base, decl, false, extras.own, why);

			for (const PrefabGroup& group : decl.groups)
			{
				Extras::Group made;
				made.name = hash_text(group.name);
				for (const PrefabEntry& entry : group.entries)
				{
					if (entry.removed)
						continue;
					const ecs::ComponentInfo* info = host.world().components().find(StringView(entry.component));
					if (info == nullptr || entry.bytes.size() != info->size)
						continue;

					// The group's fields over the prefab's own value, or the type's defaults.
					ecs::PrefabComponent component;
					component.id = info->id;
					const ecs::PrefabComponent* own = nullptr;
					for (const ecs::PrefabComponent& candidate : extras.own)
						if (candidate.id == info->id)
							own = &candidate;
					component.value = own != nullptr ? Vector<u8>(own->value.begin(), own->value.end(), &heap())
													 : Vector<u8>(info->defaults.begin(), info->defaults.end(), &heap());
					for (u32 i = 0; i < info->size; ++i)
						if (entry.mask[i] != 0)
							component.value[i] = entry.bytes[i];
					made.components.push_back(std::move(component));
				}
				extras.groups.push_back(std::move(made));
			}
			for (const PrefabEvent& event : decl.events)
			{
				Extras::Swap swap;
				swap.name = hash_text(event.name);
				const auto index_of = [&](const String& group) -> u32
				{
					for (u32 i = 0; i < decl.groups.size(); ++i)
						if (decl.groups[i].name == group)
							return i;
					return 0;
				};
				for (const String& group : event.add)
					swap.add.push_back(index_of(group));
				for (const String& group : event.remove)
					swap.remove.push_back(index_of(group));
				extras.events.push_back(std::move(swap));
			}
			brains.extras_staging.push_back(std::move(extras));
		}

		int prefab_body(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			const char* name = lua_tostring(L, lua_upvalueindex(1));
			check_declaring(L, host, "prefab");
			luaL_checktype(L, 1, LUA_TTABLE);

			if (Schema* schema = HostAccess::def(host).schema; schema != nullptr)
			{
				// Converted once every module has declared its components: finish_schema().
				Host::Brains& brains = HostAccess::brains(host);
				for (const PendingPrefab& known : brains.pending)
					if (known.name == name)
						luaL_error(L, "prefab %s is declared twice", name);
				if (host.world().prefabs().find(name) != nullptr)
					luaL_error(L, "prefab %s: a C++ prefab has that name", name);

				PendingPrefab pending;
				pending.table_ref = lua_ref(L, 1);
				pending.name	  = String(name, &heap());
				pending.path	  = String(HostAccess::loading_path(host), &heap());
				brains.pending.push_back(std::move(pending));
				return 0;
			}

			PrefabDecl decl;
			decl.path = String(HostAccess::loading_path(host), &heap());
			convert_prefab(L, host, Lookup{host, false}, name, 1, decl);

			const ecs::Prefab* registered = host.world().prefabs().find(name);
			if (registered == nullptr)
			{
				HostAccess::report(host, decl.path, 0, Severity::Warning,
								   "prefab " + decl.name + " is new: restart to apply it");
				return 0;
			}

			// New values go in live; new or missing components are for the next start.
			Vector<ecs::PrefabComponent> composed(&heap());
			switch (prefab_diff(host.world(), host.world().prefab_of(registered->id), decl, composed))
			{
				case PrefabDiff::Same:
					break;
				case PrefabDiff::Values:
					retune(host, *registered, std::move(composed));
					break;
				case PrefabDiff::Shape:
					HostAccess::report(host, decl.path, 0, Severity::Warning,
									   "prefab " + decl.name + " changed its components: restart to apply it");
					break;
			}

			record_extras(host, *registered, decl);
			return 0;
		}

		int lua_prefab(lua_State* L)
		{
			luaL_checkstring(L, 1);
			lua_pushvalue(L, 1);
			lua_pushcclosure(L, prefab_body, "prefab", 1);
			return 1;
		}

	}

	f64 declared_number(lua_State* L, int index, const char* what) { return number_or_unit(L, index, what); }

	void install_declarations(lua_State* L, Host& host)
	{
		(void)host;

		// The unit values' metatable: arithmetic and comparison as numbers, in the registry for the closures.
		lua_newtable(L);
		lua_pushcfunction(L, unit_arith<Add>, "__add");
		lua_setfield(L, -2, "__add");
		lua_pushcfunction(L, unit_arith<Sub>, "__sub");
		lua_setfield(L, -2, "__sub");
		lua_pushcfunction(L, unit_arith<Mul>, "__mul");
		lua_setfield(L, -2, "__mul");
		lua_pushcfunction(L, unit_arith<Div>, "__div");
		lua_setfield(L, -2, "__div");
		lua_pushcfunction(L, unit_arith<Neg>, "__unm");
		lua_setfield(L, -2, "__unm");
		lua_pushcfunction(L, unit_lt, "__lt");
		lua_setfield(L, -2, "__lt");
		lua_pushcfunction(L, unit_le, "__le");
		lua_setfield(L, -2, "__le");
		lua_pushcfunction(L, unit_tostring, "__tostring");
		lua_setfield(L, -2, "__tostring");
		lua_setreadonly(L, -1, true);
		const int meta = lua_ref(L, -1);
		lua_pop(L, 1);

		for (size_t i = 0; i < std::size(UNIT_NAMES); ++i)
		{
			lua_pushinteger(L, static_cast<int>(i));
			lua_pushinteger(L, meta);
			lua_pushcclosure(L, unit_call, UNIT_NAMES[i], 2);
			lua_setglobal(L, UNIT_NAMES[i]);
		}

		lua_pushcfunction(L, lua_component, "component");
		lua_setglobal(L, "component");
		lua_pushcfunction(L, lua_prefab, "prefab");
		lua_setglobal(L, "prefab");
	}

	void Host::finish_schema() noexcept
	{
		Schema* schema = m_def.schema;
		if (schema == nullptr || m_L == nullptr)
			return;

		// The collected components, laid out as the registry will lay them out, exposed by their fields so
		// a prefab's values convert through the same code a running host uses.
		Brains& brains = *m_brains;
		brains.described.clear();
		brains.described_exposed.clear();
		brains.described.reserve(schema->components.size());
		for (const ecs::DynamicComponentDef& def : schema->components)
		{
			ecs::ComponentInfo info;
			if (!ecs::describe_dynamic(def, info))
			{
				Problem problem;
				problem.message = "component " + def.name + " cannot be registered";
				schema->problems.push_back(std::move(problem));
				continue;
			}
			brains.described.push_back(std::move(info));
		}
		for (ecs::ComponentInfo& info : brains.described)
			info.name = info.own_name; // the view follows the moved string
		for (const ecs::ComponentInfo& info : brains.described)
		{
			Exposed exposed;
			exposed.info = &info;
			describe_fields(*this, info, exposed);
			brains.described_exposed.push_back(std::move(exposed));
		}

		// The prefabs, each converted on a thread of its own so a mistake in one is one problem.
		for (PendingPrefab& pending : brains.pending)
		{
			lua_State* thread = lua_newthread(m_L);
			lua_getref(thread, pending.table_ref);

			struct Args
			{
				Host* host;
				PendingPrefab* pending;
				PrefabDecl decl;
			} args{this, &pending, {}};
			args.decl.path = pending.path;

			lua_pushcfunction(
				thread,
				[](lua_State* L) -> int
				{
					Args& args = *static_cast<Args*>(lua_tolightuserdata(L, 2));
					convert_prefab(L, *args.host, Lookup{*args.host, true}, args.pending->name.c_str(), 1, args.decl);
					return 0;
				},
				"prefab");
			lua_insert(thread, 1);
			lua_pushlightuserdata(thread, &args);
			const int status = lua_pcall(thread, 2, 0, 0);

			if (status == LUA_OK)
			{
				schema->prefabs.push_back(std::move(args.decl));
			}
			else
			{
				Problem problem;
				problem.path	= pending.path;
				problem.message = String(lua_tostring(thread, -1) != nullptr ? lua_tostring(thread, -1) : "error", &heap());
				schema->problems.push_back(std::move(problem));
			}

			lua_unref(m_L, pending.table_ref);
			lua_pop(m_L, 1); // the thread
		}
		brains.pending.clear();

		// Every stategraph a prefab names was declared somewhere.
		for (const PrefabDecl& decl : schema->prefabs)
		{
			for (const PrefabEntry& entry : decl.entries)
			{
				if (entry.component != "Stategraph" || entry.removed || entry.bytes.size() < sizeof(Stategraph))
					continue;
				Stategraph value;
				std::memcpy(&value, entry.bytes.data(), sizeof(value));
				for (const Stategraph::Slot& slot : value.slots)
				{
					if (slot.graph.value == 0)
						continue;
					bool declared = false;
					for (const String& graph : schema->stategraphs)
						declared = declared || graph_id(graph) == slot.graph;
					if (!declared)
					{
						Problem problem;
						problem.path	 = decl.path;
						problem.severity = Severity::Warning;
						problem.message	 = "prefab " + decl.name + " names a stategraph no script declares";
						schema->problems.push_back(std::move(problem));
					}
				}
			}
		}

		std::sort(schema->components.begin(), schema->components.end(),
				  [](const ecs::DynamicComponentDef& a, const ecs::DynamicComponentDef& b) { return a.name < b.name; });
		std::sort(schema->prefabs.begin(), schema->prefabs.end(),
				  [](const PrefabDecl& a, const PrefabDecl& b) { return a.name < b.name; });
	}
}
