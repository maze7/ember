#include "internal.h"

#include <ember/core/logger.h>

#include <glm/ext/vector_int2_sized.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <new>

/**
 * The Binding's records, and what scripts touch through them: every entity, component and component
 * type is one lightuserdata, a 64 bit handle whose tag says which, and the one metatable they share
 * dispatches on the tag and the atom of the name asked for. Nothing is allocated to read or write a
 * field, and a loop over a query hands out handles only.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		// Handle bits. An entity is its EnTT id in the low half; a component adds its type, plus one so a
		// type of zero still differs from a bare entity; a type sets its own bit. No two kinds collide.
		constexpr u64 TYPE_BIT = u64{1} << 62;

		[[nodiscard]] void* handle(u64 bits) noexcept { return reinterpret_cast<void*>(static_cast<uintptr_t>(bits)); }
		[[nodiscard]] u64 bits_of(void* handle) noexcept { return static_cast<u64>(reinterpret_cast<uintptr_t>(handle)); }

		[[nodiscard]] ecs::Entity entity_of(u64 bits) noexcept
		{
			return static_cast<ecs::Entity>(static_cast<u32>(bits & 0xffffffffull));
		}

		[[nodiscard]] const char* kind_name(ecs::Kind kind) noexcept
		{
			if (has_any(kind, ecs::Kind::Predicted))
				return "Predicted";
			if (has_any(kind, ecs::Kind::Replicated))
				return "Replicated";
			if (has_any(kind, ecs::Kind::Client))
				return "Client";
			if (has_any(kind, ecs::Kind::Server))
				return "Server";
			return "Sim";
		}

		template <class T> [[nodiscard]] T read_as(const void* p) noexcept
		{
			T value;
			std::memcpy(&value, p, sizeof(T));
			return value;
		}

		template <class T> void store_as(void* p, T value) noexcept { std::memcpy(p, &value, sizeof(T)); }

		/** An integer field's value, whatever its width, as a double. */
		[[nodiscard]] f64 read_integer(const Field& field, const void* p) noexcept
		{
			switch (field.size)
			{
				case 1:
					return field.is_signed ? static_cast<f64>(read_as<i8>(p)) : static_cast<f64>(read_as<u8>(p));
				case 2:
					return field.is_signed ? static_cast<f64>(read_as<i16>(p)) : static_cast<f64>(read_as<u16>(p));
				case 4:
					return field.is_signed ? static_cast<f64>(read_as<i32>(p)) : static_cast<f64>(read_as<u32>(p));
				default:
					return field.is_signed ? static_cast<f64>(read_as<i64>(p)) : static_cast<f64>(read_as<u64>(p));
			}
		}

		/** The range an integer field takes, inclusive. */
		void integer_range(const Field& field, f64& low, f64& high) noexcept
		{
			switch (field.size)
			{
				case 1:
					low	 = field.is_signed ? -128.0 : 0.0;
					high = field.is_signed ? 127.0 : 255.0;
					return;
				case 2:
					low	 = field.is_signed ? -32768.0 : 0.0;
					high = field.is_signed ? 32767.0 : 65535.0;
					return;
				case 4:
					low	 = field.is_signed ? -2147483648.0 : 0.0;
					high = field.is_signed ? 2147483647.0 : 4294967295.0;
					return;
				default:
					low	 = field.is_signed ? -9223372036854775808.0 : 0.0;
					high = field.is_signed ? 9223372036854775807.0 : 18446744073709551615.0;
					return;
			}
		}

		void store_integer(const Field& field, void* p, f64 value) noexcept
		{
			switch (field.size)
			{
				case 1:
					if (field.is_signed)
						store_as<i8>(p, static_cast<i8>(value));
					else
						store_as<u8>(p, static_cast<u8>(value));
					return;
				case 2:
					if (field.is_signed)
						store_as<i16>(p, static_cast<i16>(value));
					else
						store_as<u16>(p, static_cast<u16>(value));
					return;
				case 4:
					if (field.is_signed)
						store_as<i32>(p, static_cast<i32>(value));
					else
						store_as<u32>(p, static_cast<u32>(value));
					return;
				default:
					if (field.is_signed)
						store_as<i64>(p, static_cast<i64>(value));
					else
						store_as<u64>(p, static_cast<u64>(value));
					return;
			}
		}

		/** A number at `index` that must be whole and within [low, high]. Raises, naming the field. */
		[[nodiscard]] f64 check_whole(lua_State* L, const Exposed& exposed, const Field& field, int index, f64 low,
									  f64 high, bool checks)
		{
			int is_number	= 0;
			const f64 value = lua_tonumberx(L, index, &is_number);
			if (!is_number)
				luaL_error(L, "%s.%s takes a number, not %s", exposed.info->name.data(), field.name.c_str(),
						   luaL_typename(L, index));

			if (checks && (std::floor(value) != value || value < low || value > high))
				luaL_error(L, "%s.%s takes a whole number from %.0f to %.0f, not %g", exposed.info->name.data(),
						   field.name.c_str(), low, high, value);

			return value;
		}

		[[nodiscard]] void* name_handle(u64 name) noexcept
		{
			return reinterpret_cast<void*>(static_cast<uintptr_t>(name));
		}
		[[nodiscard]] u64 name_of(void* handle) noexcept
		{
			return static_cast<u64>(reinterpret_cast<uintptr_t>(handle));
		}

		/** The exposure of a component a handle names: handles are only ever made for exposed types. */
		[[nodiscard]] const Exposed& exposed_of(lua_State* L, const Host& host, ecs::ComponentId id)
		{
			const Exposed* exposed = HostAccess::binding(host).exposed(id);
			if (exposed == nullptr)
				luaL_error(L, "component %u is not exposed to scripts", id);
			return *exposed;
		}

		// --- the shared metatable -----------------------------------------------------------------

		int handle_index(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			ecs::World& world = host.world();
			const int tag	 = lua_lightuserdatatag(L, 1);
			const u64 bits	 = bits_of(lua_tolightuserdatatagged(L, 1, tag));

			int atom			  = -1;
			const char* key		  = lua_tostringatom(L, 2, &atom);
			if (key == nullptr)
				luaL_error(L, "a %s is indexed by name, not %s", luaL_typename(L, 1), luaL_typename(L, 2));

			switch (tag)
			{
				case TAG_ENTITY:
				{
					// e.rng: the entity's dice.
					if (atom >= 0 && atom == HostAccess::brains(host).atom_rng)
					{
						lua_pushlightuserdatatagged(L, handle(HANDLE_HIGH | static_cast<u32>(entt::to_integral(live_entity(L, world, 1)))),
													TAG_RNG);
						return 1;
					}

					// e.state: its first graph's Stategraph, whose methods mean the slot being stepped.
					if (atom >= 0 && atom == HostAccess::brains(host).atom_state)
					{
						const ecs::Entity entity	   = live_entity(L, world, 1);
						const ecs::ComponentInfo* info = HostAccess::brains(host).stategraph;
						if (info == nullptr || info->find(*info, world.registry, entity) == nullptr)
							lua_pushnil(L);
						else
							push_component(L, entity, info->id);
						return 1;
					}

					// e.Position: the component handle, or nil when the entity has none: `if e.Push then`.
					const ecs::ComponentId id = HostAccess::component_of_atom(host, atom);
					if (id == ecs::NO_COMPONENT)
						luaL_error(L, "'%s' is not a component; an entity's components are reached by their names", key);

					const ecs::Entity entity = live_entity(L, world, 1);
					const ecs::ComponentInfo& info = world.components()[id];
					if (info.find(info, world.registry, entity) == nullptr)
						lua_pushnil(L);
					else
						push_component(L, entity, id);
					return 1;
				}

				case TAG_COMPONENT:
				{
					const auto id		   = static_cast<ecs::ComponentId>((bits >> 32) & 0xffff) - 1;
					const Exposed& exposed = exposed_of(L, host, static_cast<ecs::ComponentId>(id));
					const Field* field	   = exposed.find(static_cast<i16>(atom));
					if (field == nullptr)
						luaL_error(L, "%s has no field '%s'; it has: %s", exposed.info->name.data(), key,
								   exposed.field_list.c_str());

					const ecs::Entity entity = live_entity(L, world, 1);
					const void* bytes		 = exposed.info->find(*exposed.info, world.registry, entity);
					if (bytes == nullptr)
						luaL_error(L, "the entity no longer has a %s", exposed.info->name.data());

					++HostAccess::stats(host).reads;
					push_field(L, host, *field, bytes, entity, static_cast<ecs::ComponentId>(id));
					return 1;
				}

				case TAG_NAME:
					luaL_error(
						L, "a Name has no fields; compare it, print it, or hand it to play(), spawn() and the rest");

				case TAG_COMPONENT_TYPE:
				{
					const Exposed& exposed = exposed_of(L, host, static_cast<ecs::ComponentId>(bits & 0xffff));
					if (StringView(key) == "name")
					{
						lua_pushlstring(L, exposed.info->name.data(), exposed.info->name.size());
						return 1;
					}
					luaL_error(L, "%s is a component type: pass it to a query, has(), add() or remove()",
							   exposed.info->name.data());
				}

				default:
					luaL_error(L, "cannot index a %s", luaL_typename(L, 1));
			}
		}

		int handle_newindex(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			ecs::World& world = host.world();
			const int tag	 = lua_lightuserdatatag(L, 1);
			if (tag != TAG_COMPONENT)
				luaL_error(L, "a %s cannot be written; write a component's fields", luaL_typename(L, 1));

			const u64 bits		   = bits_of(lua_tolightuserdatatagged(L, 1, tag));
			const auto id		   = static_cast<ecs::ComponentId>(((bits >> 32) & 0xffff) - 1);
			const Exposed& exposed = exposed_of(L, host, id);

			int atom		= -1;
			const char* key = lua_tostringatom(L, 2, &atom);
			if (key == nullptr)
				luaL_error(L, "%s is written by field name, not %s", exposed.info->name.data(), luaL_typename(L, 2));

			const Field* field = exposed.find(static_cast<i16>(atom));
			if (field == nullptr)
				luaL_error(L, "%s has no field '%s'; it has: %s", exposed.info->name.data(), key,
						   exposed.field_list.c_str());

			const Context context = HostAccess::context(host);
			if (!may_write(context, exposed))
				refuse_write(L, context, exposed, field->name);

			const ecs::Entity entity = live_entity(L, world, 1);
			check_sim_target(L, host, entity);
			void* bytes = exposed.info->get(*exposed.info, world.registry, entity);
			if (bytes == nullptr)
				luaL_error(L, "the entity no longer has a %s", exposed.info->name.data());

			++HostAccess::stats(host).writes;
			write_field(L, exposed, *field, bytes, 3, HostAccess::def(host).checks, entity);
			return 0;
		}

		/** e:add(T, fields?), e:remove(T): a component's bytes made and handed to the commands. */
		int entity_add(lua_State* L, Host& host, ecs::Entity entity)
		{
			const ecs::ComponentId id = check_component_type(L, 2);
			const Exposed& exposed	  = exposed_of(L, host, id);
			const Context context	  = HostAccess::context(host);
			if (!may_write(context, exposed))
				refuse_write(L, context, exposed, "add");
			check_sim_target(L, host, entity);

			ecs::Commands& commands = check_commands(L, host, "add");

			// The type's defaults, then the fields given, in a buffer on the stack for the usual sizes.
			const ecs::ComponentInfo& info = *exposed.info;
			alignas(16) u8 small[256];
			Vector<u8> large(&heap());
			u8* bytes = small;
			if (info.size > sizeof(small))
			{
				large.resize(info.size);
				bytes = large.data();
			}
			if (info.size > 0)
				std::memcpy(bytes, info.defaults.data(), info.size);

			if (!lua_isnoneornil(L, 3))
			{
				luaL_checktype(L, 3, LUA_TTABLE);
				fill_component(L, exposed, 3, bytes, HostAccess::def(host).checks);
			}

			commands.add(entity, id, bytes);
			++HostAccess::stats(host).writes;
			return 0;
		}

		int entity_remove(lua_State* L, Host& host, ecs::Entity entity)
		{
			const ecs::ComponentId id = check_component_type(L, 2);
			const Exposed& exposed	  = exposed_of(L, host, id);
			const Context context	  = HostAccess::context(host);
			if (!may_write(context, exposed))
				refuse_write(L, context, exposed, "remove");
			check_sim_target(L, host, entity);

			check_commands(L, host, "remove").remove(entity, id);
			++HostAccess::stats(host).writes;
			return 0;
		}

		int handle_namecall(lua_State* L)
		{
			Host& host		 = HostAccess::of(L);
			ecs::World& world = host.world();
			const int tag	 = lua_lightuserdatatag(L, 1);
			const u64 bits	 = bits_of(lua_tolightuserdatatagged(L, 1, tag));

			int atom		   = -1;
			const char* method = lua_namecallatom(L, &atom);
			if (method == nullptr)
				luaL_error(L, "a method is called by name");

			switch (tag)
			{
				case TAG_ENTITY:
				{
					const i16 verb = HostAccess::verb_of_atom(host, atom);

					// exists() is the one question a dead handle may be asked.
					if (static_cast<Verb>(verb) == Verb::Exists)
					{
						lua_pushboolean(L, world.registry.valid(entity_of(bits)));
						return 1;
					}
					const ecs::Entity entity = live_entity(L, world, 1);

					switch (static_cast<Verb>(verb))
					{
						case Verb::Has:
						{
							const ecs::ComponentId id = check_component_type(L, 2);
							const ecs::ComponentInfo& info = world.components()[id];
							lua_pushboolean(L, info.find(info, world.registry, entity) != nullptr);
							return 1;
						}
						case Verb::Add:
							return entity_add(L, host, entity);
						case Verb::Remove:
							return entity_remove(L, host, entity);
						case Verb::Destroy:
							check_context(L, host, ContextMask::Server, "destroy");
							check_commands(L, host, "destroy").destroy(entity);
							return 0;
						case Verb::Id:
							lua_pushnumber(L, static_cast<f64>(entt::to_integral(entity)));
							return 1;
						case Verb::Play:
							return entity_play(L, host, entity);
						case Verb::Event:
							return entity_event(L, host, entity);
						case Verb::Flash:
						case Verb::Overlay:
						case Verb::Squash:
						case Verb::Lean:
						case Verb::Hold:
						case Verb::Sound:
						case Verb::Mine:
							return client_verb(L, host, entity, static_cast<Verb>(verb));
						case Verb::Strike:
							return entity_strike(L, host, entity);
						case Verb::Start:
							return entity_start(L, host, entity);
						case Verb::Stop:
							return entity_stop(L, host, entity);
						case Verb::Inflict:
							return entity_inflict(L, host, entity);
						case Verb::Cure:
							return entity_cure(L, host, entity);
						case Verb::Modifier:
							return entity_modifier(L, host, entity);
						case Verb::Stat:
							return entity_stat(L, host, entity);
						case Verb::Prefab:
						{
							// The prefab it was made from, by name; nil for one made bare.
							const ecs::PrefabRef* ref = world.registry.try_get<ecs::PrefabRef>(entity);
							if (ref == nullptr)
								lua_pushnil(L);
							else
							{
								const StringView name = world.prefabs()[ref->id].name;
								lua_pushlstring(L, name.data(), name.size());
							}
							return 1;
						}
						case Verb::Exists:
						case Verb::None:
							luaL_error(L, "an entity has no method '%s'", method);
						default:
						{
							// A game's own: its Function, called with the entity as argument 1.
							const Span<const Function> methods = HostAccess::binding(host).entity_methods();
							const auto index				   = static_cast<size_t>(verb - static_cast<i16>(Verb::Count));
							if (index >= methods.size())
								luaL_error(L, "an entity has no method '%s'", method);

							const Function& function = methods[index];
							check_context(L, host, function.where, function.name);
							return function.call(L);
						}
					}
				}

				case TAG_RNG:
					return rng_namecall(L);

				case TAG_COMPONENT:
				{
					const auto id			 = static_cast<ecs::ComponentId>(((bits >> 32) & 0xffff) - 1);
					const Exposed& exposed	 = exposed_of(L, host, id);
					const Method* found		 = exposed.find_method(static_cast<i16>(atom));
					if (found == nullptr)
						luaL_error(L, "%s has no method '%s'", exposed.info->name.data(), method);

					const ecs::Entity entity = live_entity(L, world, 1);
					void* bytes				 = exposed.info->get(*exposed.info, world.registry, entity);
					if (bytes == nullptr)
						luaL_error(L, "the entity no longer has a %s", exposed.info->name.data());

					return found->call(L, bytes, entity);
				}

				default:
					luaL_error(L, "a %s has no methods", luaL_typename(L, 1));
			}
		}

		int handle_tostring(lua_State* L)
		{
			Host& host	   = HostAccess::of(L);
			const int tag  = lua_lightuserdatatag(L, 1);
			const u64 bits = bits_of(lua_tolightuserdatatagged(L, 1, tag));

			switch (tag)
			{
				case TAG_ENTITY:
					lua_pushfstring(L, "Entity(%u)", static_cast<unsigned>(entt::to_integral(entity_of(bits))));
					return 1;
				case TAG_COMPONENT:
				{
					const auto id			 = static_cast<ecs::ComponentId>(((bits >> 32) & 0xffff) - 1);
					const Exposed* exposed	 = HostAccess::binding(host).exposed(id);
					lua_pushfstring(L, "%s of Entity(%u)", exposed != nullptr ? exposed->info->name.data() : "?",
									static_cast<unsigned>(entt::to_integral(entity_of(bits))));
					return 1;
				}
				case TAG_COMPONENT_TYPE:
				{
					const Exposed* exposed = HostAccess::binding(host).exposed(static_cast<ecs::ComponentId>(bits & 0xffff));
					lua_pushfstring(L, "Component %s", exposed != nullptr ? exposed->info->name.data() : "?");
					return 1;
				}
				case TAG_RNG:
					lua_pushfstring(L, "Rng of Entity(%u)", static_cast<unsigned>(entt::to_integral(entity_of(bits))));
					return 1;
				case TAG_NAME:
				{
					const StringView text = name_text(L, bits);
					if (text.empty())
						lua_pushfstring(L, "Name(%016llx)", static_cast<unsigned long long>(bits));
					else
						lua_pushlstring(L, text.data(), text.size());
					return 1;
				}
				default:
					lua_pushstring(L, "userdata");
					return 1;
			}
		}
	}

	// --- Binding ----------------------------------------------------------------------------------

	Binding::Binding(ecs::World& world) noexcept
		: m_world(world), m_exposed(&heap()), m_libraries(&heap()), m_entity_methods(&heap()), m_globals(&heap()),
		  m_enumerations(&heap()), m_constants(&heap()), m_stages(&heap()), m_userdata(&heap()), m_definitions(&heap())
	{
		m_exposed.resize(world.components().count());
	}

	const Field* Exposed::find(i16 atom) const noexcept
	{
		if (atom < 0)
			return nullptr;
		for (const Field& field : fields)
			if (field.atom == atom)
				return &field;
		return nullptr;
	}

	const Field* Exposed::find(StringView name) const noexcept
	{
		for (const Field& field : fields)
			if (field.name == name)
				return &field;
		return nullptr;
	}

	const Method* Exposed::find_method(i16 atom) const noexcept
	{
		if (atom < 0)
			return nullptr;
		for (const Method& method : methods)
			if (method.atom == atom)
				return &method;
		return nullptr;
	}

	Exposed& Binding::exposed_of(const ecs::ComponentInfo& info) noexcept
	{
		Exposed& exposed = m_exposed[info.id];
		exposed.info	 = &info;
		return exposed;
	}

	const Exposed* Binding::exposed(ecs::ComponentId id) const noexcept
	{
		return id < m_exposed.size() && m_exposed[id].info != nullptr ? &m_exposed[id] : nullptr;
	}

	void Binding::library(const char* name, Span<const Function> functions) noexcept
	{
		Library library;
		library.name = String(name, &heap());
		library.functions.assign(functions.begin(), functions.end());

		// Its messages name the table too: "fx.spawn". The labels never move once made.
		library.labels.reserve(library.functions.size());
		for (Function& function : library.functions)
		{
			library.labels.push_back(library.name + "." + function.name);
			function.label = library.labels.back().c_str();
		}
		m_libraries.push_back(std::move(library));
	}

	void Binding::entity_method(const Function& function) noexcept { m_entity_methods.push_back(function); }

	void Binding::global(const Function& function) noexcept { m_globals.push_back(function); }

	void Binding::constant(const char* name, f64 value) noexcept
	{
		m_constants.push_back({.name = String(name, &heap()), .value = value});
	}

	void Binding::stage(const char* name, u8 index) noexcept
	{
		EMBER_ASSERT(StringView(name) != "Present" && "Present is the client's own stage");
		m_stages.push_back({.name = String(name, &heap()), .index = index});
	}

	void Binding::userdata(int tag, const char* type_name, lua_CFunction namecall) noexcept
	{
		m_userdata.push_back({.tag = tag, .type_name = String(type_name, &heap()), .namecall = namecall});
	}

	void Binding::definitions(const char* text) noexcept { m_definitions.push_back(String(text, &heap())); }

	void Binding::world_function(const Function& function) noexcept { m_world_functions.push_back(function); }

	void Binding::units(f64 texels_per_tile, f64 ticks_per_second) noexcept
	{
		m_texels_per_tile  = texels_per_tile;
		m_ticks_per_second = ticks_per_second;
	}

	void Binding::expose_dynamic(const ecs::ComponentInfo& info) noexcept
	{
		EMBER_ASSERT(info.dynamic);
		Exposed& exposed = exposed_of(info);
		exposed.fields.clear();
		exposed.field_list.clear();
		for (const ecs::FieldInfo& layout : info.fields)
		{
			Field field;
			field.name	 = String(layout.name, &heap());
			field.offset = layout.offset;
			field.size	 = ecs::field_size(layout.type);
			switch (layout.type)
			{
				case ecs::FieldType::Bool:
					field.kind = FieldKind::Bool;
					break;
				case ecs::FieldType::F32:
					field.kind = FieldKind::Float;
					break;
				case ecs::FieldType::Vec2:
					field.kind = FieldKind::Vec2;
					break;
				case ecs::FieldType::I32:
					field.kind		= FieldKind::Int;
					field.is_signed = true;
					break;
				case ecs::FieldType::Entity:
					field.kind = FieldKind::EntityRef;
					break;
				case ecs::FieldType::Name:
					field.kind = FieldKind::Name;
					break;
				default:
					field.kind = FieldKind::Int;
					break;
			}
			if (!exposed.field_list.empty())
				exposed.field_list += ' ';
			exposed.field_list += field.name;
			exposed.fields.push_back(std::move(field));
		}
	}

	void describe_fields(Host& host, const ecs::ComponentInfo& info, Exposed& exposed) noexcept
	{
		// As expose_dynamic() lays them out, with atoms, for a type the registry does not have yet.
		Binding& binding = HostAccess::binding(host);
		Exposed scratch;
		scratch.info = &info;
		// The binding's exposure table is by id, which this type has not got: build the fields alongside it.
		for (const ecs::FieldInfo& layout : info.fields)
		{
			Field field;
			field.name	 = String(layout.name, &heap());
			field.offset = layout.offset;
			field.size	 = ecs::field_size(layout.type);
			field.kind		= layout.type == ecs::FieldType::Bool	  ? FieldKind::Bool
							  : layout.type == ecs::FieldType::F32	  ? FieldKind::Float
							  : layout.type == ecs::FieldType::Vec2	  ? FieldKind::Vec2
							  : layout.type == ecs::FieldType::Entity ? FieldKind::EntityRef
							  : layout.type == ecs::FieldType::Name	  ? FieldKind::Name
																	  : FieldKind::Int;
			field.is_signed = layout.type == ecs::FieldType::I32;
			field.atom		= HostAccess::atom(host, field.name);
			if (!exposed.field_list.empty())
				exposed.field_list += ' ';
			exposed.field_list += field.name;
			exposed.fields.push_back(std::move(field));
		}
		(void)binding;
	}

	bool runs_in(Context context, ecs::Role role) noexcept
	{
		switch (context)
		{
			case Context::Server:
				return role != ecs::Role::Client;
			case Context::Client:
				return role != ecs::Role::Server;
			case Context::Sim:
				return true;
			default:
				return false;
		}
	}

	const StageName* Binding::find_stage(StringView name) const noexcept
	{
		for (const StageName& stage : m_stages)
			if (stage.name == name)
				return &stage;
		return nullptr;
	}

	// --- rights -----------------------------------------------------------------------------------

	bool may_write(Context context, const Exposed& exposed) noexcept
	{
		const ecs::Kind kind = exposed.info->kind;
		switch (context)
		{
			case Context::Sim:
				// What the owner's client simulates ahead and the server corrects; and what is recomputed
				// every tick, which nobody carries across.
				return exposed.derived || has_any(kind, ecs::Kind::Predicted);
			case Context::Server:
				return !has_any(kind, ecs::Kind::Client);
			case Context::Client:
				return exposed.derived || has_any(kind, ecs::Kind::Client);
			default:
				return false;
		}
	}

	void refuse_write(lua_State* L, Context context, const Exposed& exposed, StringView what)
	{
		const char* name = exposed.info->name.data();
		switch (context)
		{
			case Context::Sim:
				luaL_error(L,
						   "%s.%.*s: a sim script writes Predicted components only, which every machine simulates "
						   "alike; %s is %s, so this belongs in scripts/server",
						   name, static_cast<int>(what.size()), what.data(), name, kind_name(exposed.info->kind));
			case Context::Client:
				luaL_error(L, "%s.%.*s: a client script writes Client components only; %s is game state", name,
						   static_cast<int>(what.size()), what.data(), name);
			case Context::Server:
				luaL_error(L, "%s.%.*s: a server has no %s components", name, static_cast<int>(what.size()),
						   what.data(), kind_name(exposed.info->kind));
			default:
				luaL_error(L, "%s.%.*s: the world is not written while a module loads; do it in a system", name,
						   static_cast<int>(what.size()), what.data());
		}
	}

	void check_context(lua_State* L, const Host& host, ContextMask where, const char* name)
	{
		const Context context = HostAccess::context(host);
		if (context == Context::Count)
			luaL_error(L, "%s: not while a module loads; call it from a system", name);
		if (!has_any(where, mask_of(context)))
			luaL_error(L, "%s: not from a %s script", name, enum_name(context));
	}

	ecs::Commands& check_commands(lua_State* L, Host& host, const char* what)
	{
		ecs::Commands* commands = HostAccess::commands(host);
		if (commands == nullptr)
			luaL_error(L, "%s: not while a module loads; call it from a system", what);
		return *commands;
	}

	void check_sim_target(lua_State* L, Host& host, ecs::Entity entity)
	{
		ecs::World& world = host.world();
		if (HostAccess::context(host) == Context::Sim && world.role() == ecs::Role::Client &&
			!world.registry.all_of<ecs::Simulated>(entity))
			luaL_error(L,
					   "Entity(%u) is not simulated on this client: a sim script writes only what this machine "
					   "predicts, its own player; the server's word would undo this, and nothing would replay it",
					   static_cast<unsigned>(entt::to_integral(entity)));
	}

	ecs::Entity entity_of_netid(Host& host, u32 id) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		if (!brains.netids_built)
		{
			brains.by_netid.clear();
			for (const auto [entity, netid] : host.world().registry.view<const net::NetId>().each())
				brains.by_netid.insert_or_assign(netid.value, entity);
			brains.netids_built = true;
		}
		const auto found = brains.by_netid.find(id);
		return found != brains.by_netid.end() && host.world().registry.valid(found->second) ? found->second
																							: ecs::NO_ENTITY;
	}

	u32 netid_of(Host& host, ecs::Entity entity) noexcept
	{
		const net::NetId* id = host.world().registry.try_get<net::NetId>(entity);
		return id != nullptr ? id->value : 0;
	}

	void settle_pending_refs(Host& host) noexcept
	{
		Host::Brains& brains = HostAccess::brains(host);
		ecs::World& world	 = host.world();
		std::erase_if(brains.pending_refs,
					  [&](const PendingRef& ref)
					  {
						  if (!world.registry.valid(ref.holder) || !world.registry.valid(ref.target))
							  return true;
						  const u32 id = netid_of(host, ref.target);
						  if (id == 0)
							  return false;
						  const ecs::ComponentInfo& info = world.components()[ref.component];
						  if (void* bytes = info.get(info, world.registry, ref.holder))
							  std::memcpy(static_cast<u8*>(bytes) + ref.offset, &id, sizeof(id));
						  return true;
					  });
	}

	u64 intern_name(Host& host, StringView text) noexcept
	{
		const u64 name		 = hash_text(text);
		Host::Brains& brains = HostAccess::brains(host);
		if (name != 0 && !brains.names.contains(name))
			brains.names.insert_or_assign(name, String(text, &heap()));
		return name;
	}

	void push_name(lua_State* L, u64 name) noexcept
	{
		if (name == 0)
			lua_pushnil(L);
		else
			lua_pushlightuserdatatagged(L, name_handle(name), TAG_NAME);
	}

	u64 check_name(lua_State* L, int index)
	{
		if (lua_type(L, index) == LUA_TSTRING)
			return intern_name(HostAccess::of(L), lua_tostring(L, index));
		if (lua_lightuserdatatag(L, index) == TAG_NAME)
			return name_of(lua_tolightuserdatatagged(L, index, TAG_NAME));
		luaL_typeerror(L, index, "a name: a string, or a Name");
	}

	u64 opt_name(lua_State* L, int index) { return lua_isnoneornil(L, index) ? 0 : check_name(L, index); }

	StringView name_text(lua_State* L, u64 name) noexcept
	{
		const Host::Brains& brains = HostAccess::brains(HostAccess::of(L));
		const auto found		   = brains.names.find(name);
		return found != brains.names.end() ? StringView(found->second) : StringView();
	}

	ecs::Entity live_entity(lua_State* L, ecs::World& world, int index)
	{
		const int tag = lua_lightuserdatatag(L, index);
		if (tag != TAG_ENTITY && tag != TAG_COMPONENT && tag != TAG_RNG)
			luaL_typeerror(L, index, "Entity");

		const ecs::Entity entity = entity_of(bits_of(lua_tolightuserdatatagged(L, index, tag)));
		if (!world.registry.valid(entity))
			luaL_error(L, "Entity(%u) is gone", static_cast<unsigned>(entt::to_integral(entity)));
		return entity;
	}

	// --- fields -----------------------------------------------------------------------------------

	void push_field(lua_State* L, const Field& field, const void* bytes)
	{
		const void* p = static_cast<const u8*>(bytes) + field.offset;
		switch (field.kind)
		{
			case FieldKind::Bool:
				lua_pushboolean(L, read_as<bool>(p));
				return;
			case FieldKind::Int:
			case FieldKind::Enum:
				lua_pushnumber(L, read_integer(field, p));
				return;
			case FieldKind::Float:
				lua_pushnumber(L, field.size == 4 ? static_cast<f64>(read_as<f32>(p)) : read_as<f64>(p));
				return;
			case FieldKind::Vec2:
			{
				const glm::vec2 value = read_as<glm::vec2>(p);
				lua_pushvector(L, value.x, value.y, 0.0f);
				return;
			}
			case FieldKind::Vec2i8:
			{
				const glm::i8vec2 value = read_as<glm::i8vec2>(p);
				lua_pushvector(L, static_cast<f32>(value.x), static_cast<f32>(value.y), 0.0f);
				return;
			}
			case FieldKind::Layers:
				lua_pushnumber(L, static_cast<f64>(read_as<physics::Layers>(p).bits));
				return;
			case FieldKind::Shape:
				push_shape(L, read_as<physics::Shape>(p));
				return;
			case FieldKind::Text:
			{
				const char* text = read_as<const char*>(p);
				if (text == nullptr)
					lua_pushnil(L);
				else
					lua_pushstring(L, text);
				return;
			}
			case FieldKind::EntityRef:
				lua_pushnumber(L, read_as<u32>(p));
				return;
			case FieldKind::Name:
				push_name(L, read_as<u64>(p));
				return;
			default:
				lua_pushnil(L);
				return;
		}
	}

	void push_field(lua_State* L, Host& host, const Field& field, const void* bytes, ecs::Entity holder,
					ecs::ComponentId component)
	{
		if (field.kind != FieldKind::EntityRef)
		{
			push_field(L, field, bytes);
			return;
		}

		// By its id; or, while the target waits for one, the pending ref.
		const u32 id	   = read_as<u32>(static_cast<const u8*>(bytes) + field.offset);
		ecs::Entity entity = id != 0 ? entity_of_netid(host, id) : ecs::NO_ENTITY;
		if (entity == ecs::NO_ENTITY && id == 0)
			for (const PendingRef& ref : HostAccess::brains(host).pending_refs)
				if (ref.holder == holder && ref.component == component && ref.offset == field.offset)
					entity = host.world().registry.valid(ref.target) ? ref.target : ecs::NO_ENTITY;
		if (entity == ecs::NO_ENTITY)
			lua_pushnil(L);
		else
			push_entity(L, entity);
	}

	void write_field(lua_State* L, const Exposed& exposed, const Field& field, void* bytes, int index, bool checks,
					 ecs::Entity holder)
	{
		if (index < 0)
			index = lua_gettop(L) + 1 + index;

		// A unit value from the schema pass, tiles(3), stands for its number.
		if (lua_istable(L, index))
		{
			lua_rawgetfield(L, index, "__unit");
			const bool unit = !lua_isnil(L, -1);
			lua_pop(L, 1);
			if (unit)
			{
				lua_rawgetfield(L, index, "n");
				lua_replace(L, index);
			}
		}

		void* p = static_cast<u8*>(bytes) + field.offset;
		switch (field.kind)
		{
			case FieldKind::Bool:
				if (!lua_isboolean(L, index))
					luaL_error(L, "%s.%s takes true or false, not %s", exposed.info->name.data(), field.name.c_str(),
							   luaL_typename(L, index));
				store_as<bool>(p, lua_toboolean(L, index) != 0);
				return;

			case FieldKind::Int:
			case FieldKind::Enum:
			{
				f64 low, high;
				integer_range(field, low, high);
				store_integer(field, p, check_whole(L, exposed, field, index, low, high, checks));
				return;
			}

			case FieldKind::Float:
			{
				int is_number	= 0;
				const f64 value = lua_tonumberx(L, index, &is_number);
				if (!is_number)
					luaL_error(L, "%s.%s takes a number, not %s", exposed.info->name.data(), field.name.c_str(),
							   luaL_typename(L, index));
				if (field.size == 4)
					store_as<f32>(p, static_cast<f32>(value));
				else
					store_as<f64>(p, value);
				return;
			}

			case FieldKind::Vec2:
			{
				const f32* v = lua_tovector(L, index);
				if (v == nullptr)
					luaL_error(L, "%s.%s takes a vector, not %s", exposed.info->name.data(), field.name.c_str(),
							   luaL_typename(L, index));
				store_as<glm::vec2>(p, {v[0], v[1]});
				return;
			}

			case FieldKind::Vec2i8:
			{
				const f32* v = lua_tovector(L, index);
				if (v == nullptr)
					luaL_error(L, "%s.%s takes a vector, not %s", exposed.info->name.data(), field.name.c_str(),
							   luaL_typename(L, index));
				if (checks && (std::floor(v[0]) != v[0] || std::floor(v[1]) != v[1] || v[0] < -128.0f ||
							   v[0] > 127.0f || v[1] < -128.0f || v[1] > 127.0f))
					luaL_error(L, "%s.%s takes whole components from -128 to 127, not (%g, %g)",
							   exposed.info->name.data(), field.name.c_str(), v[0], v[1]);
				store_as<glm::i8vec2>(p, {static_cast<i8>(v[0]), static_cast<i8>(v[1])});
				return;
			}

			case FieldKind::Layers:
			{
				physics::Layers layers;
				layers.bits = static_cast<u32>(check_whole(L, exposed, field, index, 0.0, 4294967295.0, checks));
				store_as<physics::Layers>(p, layers);
				return;
			}

			case FieldKind::Shape:
				store_shape(p, check_shape(L, index)); // padding zeroed: a prefab's bytes agree on every machine
				return;

			case FieldKind::Text:
				luaL_error(L, "%s.%s is read only", exposed.info->name.data(), field.name.c_str());

			case FieldKind::EntityRef:
			{
				// nil, or entity(), clears it. An entity is kept by its network id; one that has none yet is written
				// once it does.
				if (lua_isnil(L, index) || lua_lightuserdatatag(L, index) == TAG_NO_ENTITY)
				{
					store_as<u32>(p, 0);
					return;
				}
				Host& host				 = HostAccess::of(L);
				const ecs::Entity target = live_entity(L, host.world(), index);
				const u32 id			 = netid_of(host, target);
				if (id != 0)
				{
					store_as<u32>(p, id);
					return;
				}
				if (holder == ecs::NO_ENTITY)
					luaL_error(L, "%s.%s: Entity(%u) has no network id yet; it has one from the next tick",
							   exposed.info->name.data(), field.name.c_str(),
							   static_cast<unsigned>(entt::to_integral(target)));
				Host::Brains& brains = HostAccess::brains(host);
				std::erase_if(brains.pending_refs,
							  [&](const PendingRef& ref)
							  {
								  return ref.holder == holder && ref.component == exposed.info->id &&
										 ref.offset == field.offset;
							  });
				brains.pending_refs.push_back(
					{.holder = holder, .component = exposed.info->id, .offset = field.offset, .target = target});
				store_as<u32>(p, 0);
				return;
			}

			case FieldKind::Name:
				store_as<u64>(p, opt_name(L, index));
				return;

			default:
				luaL_error(L, "%s.%s cannot be written from a script", exposed.info->name.data(), field.name.c_str());
		}
	}

	void fill_component(lua_State* L, const Exposed& exposed, int table, void* bytes, bool checks, u8* written)
	{
		// Absolute, so the key and value lua_next leaves on top do not move it.
		if (table < 0)
			table = lua_gettop(L) + 1 + table;

		lua_pushnil(L);
		while (lua_next(L, table) != 0)
		{
			int atom		= -1;
			const char* key = lua_tostringatom(L, -2, &atom);
			if (key == nullptr)
				luaL_error(L, "%s takes fields by name, not %s", exposed.info->name.data(), luaL_typename(L, -2));

			// By atom, or by name for a string the VM fixed an atom for before the field had one: a constant of a
			// module loaded in the schema pass, whose components were not yet known.
			const Field* field = exposed.find(static_cast<i16>(atom));
			if (field == nullptr)
				field = exposed.find(StringView(key));
			if (field == nullptr)
				luaL_error(L, "%s has no field '%s'; it has: %s", exposed.info->name.data(), key,
						   exposed.field_list.c_str());

			write_field(L, exposed, *field, bytes, lua_gettop(L), checks);
			if (written != nullptr)
				std::memset(written + field->offset, 1, field->size);
			lua_pop(L, 1);
		}
	}

	// --- handles ----------------------------------------------------------------------------------

	Host& host_of(lua_State* L) noexcept { return HostAccess::of(L); }

	ecs::World& world_of(lua_State* L) noexcept { return HostAccess::of(L).world(); }

	void push_entity(lua_State* L, ecs::Entity entity) noexcept
	{
		lua_pushlightuserdatatagged(L, handle(HANDLE_HIGH | static_cast<u32>(entt::to_integral(entity))), TAG_ENTITY);
	}

	ecs::Entity to_entity(lua_State* L, int index) noexcept
	{
		const int tag = lua_lightuserdatatag(L, index);
		if (tag != TAG_ENTITY && tag != TAG_COMPONENT)
			return ecs::NO_ENTITY;
		return entity_of(bits_of(lua_tolightuserdatatagged(L, index, tag)));
	}

	ecs::Entity check_entity(lua_State* L, int index) { return live_entity(L, HostAccess::of(L).world(), index); }

	void push_component(lua_State* L, ecs::Entity entity, ecs::ComponentId component) noexcept
	{
		const u64 bits = HANDLE_HIGH | (static_cast<u64>(component) + 1) << 32 | static_cast<u32>(entt::to_integral(entity));
		lua_pushlightuserdatatagged(L, handle(bits), TAG_COMPONENT);
	}

	bool to_component(lua_State* L, int index, ecs::Entity& entity, ecs::ComponentId& component) noexcept
	{
		if (lua_lightuserdatatag(L, index) != TAG_COMPONENT)
			return false;
		const u64 bits = bits_of(lua_tolightuserdatatagged(L, index, TAG_COMPONENT));
		entity		   = entity_of(bits);
		component	   = static_cast<ecs::ComponentId>(((bits >> 32) & 0xffff) - 1);
		return true;
	}

	void push_component_type(lua_State* L, ecs::ComponentId component) noexcept
	{
		lua_pushlightuserdatatagged(L, handle(HANDLE_HIGH | TYPE_BIT | component), TAG_COMPONENT_TYPE);
	}

	ecs::ComponentId to_component_type(lua_State* L, int index) noexcept
	{
		if (lua_lightuserdatatag(L, index) != TAG_COMPONENT_TYPE)
			return ecs::NO_COMPONENT;
		return static_cast<ecs::ComponentId>(bits_of(lua_tolightuserdatatagged(L, index, TAG_COMPONENT_TYPE)) & 0xffff);
	}

	ecs::ComponentId check_component_type(lua_State* L, int index)
	{
		const ecs::ComponentId id = to_component_type(L, index);
		if (id == ecs::NO_COMPONENT)
			luaL_typeerror(L, index, "component type");
		return id;
	}

	void push_vec2(lua_State* L, glm::vec2 value) noexcept { lua_pushvector(L, value.x, value.y, 0.0f); }

	glm::vec2 check_vec2(lua_State* L, int index)
	{
		const f32* v = luaL_checkvector(L, index);
		return {v[0], v[1]};
	}

	glm::vec2 opt_vec2(lua_State* L, int index, glm::vec2 fallback)
	{
		if (lua_isnoneornil(L, index))
			return fallback;
		return check_vec2(L, index);
	}

	void install_handles(lua_State* L, Host& host)
	{
		(void)host;

		// One metatable for every lightuserdata: the tag tells them apart inside each metamethod.
		lua_newtable(L);
		lua_pushcfunction(L, handle_index, "__index");
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, handle_newindex, "__newindex");
		lua_setfield(L, -2, "__newindex");
		lua_pushcfunction(L, handle_namecall, "__namecall");
		lua_setfield(L, -2, "__namecall");
		lua_pushcfunction(L, handle_tostring, "__tostring");
		lua_setfield(L, -2, "__tostring");
		lua_setreadonly(L, -1, true);

		lua_pushlightuserdatatagged(L, handle(HANDLE_HIGH), TAG_ENTITY);
		lua_insert(L, -2);
		lua_setmetatable(L, -2);
		lua_pop(L, 1);

		lua_setlightuserdataname(L, TAG_ENTITY, "Entity");
		lua_setlightuserdataname(L, TAG_COMPONENT, "Component");
		lua_setlightuserdataname(L, TAG_COMPONENT_TYPE, "ComponentType");
		lua_setlightuserdataname(L, TAG_NAME, "Name");
		lua_setlightuserdataname(L, TAG_NO_ENTITY, "NoEntity");
	}
}
