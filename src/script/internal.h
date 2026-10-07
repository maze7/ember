#pragma once

#include <ember/script/binding.h>
#include <ember/script/host.h>
#include <ember/script/lua.h>

/**
 * What the module's translation units share and nothing outside it sees: the host's private state,
 * reached through a friend, and the installers each file contributes to the VM.
 */
namespace ember::script
{
	/** Components a query names at most, and filters leave out at most. */
	inline constexpr u32 MAX_QUERY = 8;

	/** The engine's own entity verbs, by the atom of their name; a game's entity methods follow them. */
	enum class Verb : i16
	{
		None	= -1,
		Has		= 0,
		Add		= 1,
		Remove	= 2,
		Destroy = 3,
		Id		= 4,
		Count	= 5,
	};

	struct HostAccess
	{
		[[nodiscard]] static Host& of(lua_State* L) noexcept { return *static_cast<Host*>(lua_callbacks(L)->userdata); }
		[[nodiscard]] static Context context(const Host& host) noexcept { return host.m_context; }
		[[nodiscard]] static ecs::Commands* commands(Host& host) noexcept { return host.m_commands; }
		[[nodiscard]] static bool loading(const Host& host) noexcept { return host.m_loading >= 0; }
		[[nodiscard]] static Stats& stats(Host& host) noexcept { return host.m_stats; }
		[[nodiscard]] static const HostDef& def(const Host& host) noexcept { return host.m_def; }
		[[nodiscard]] static const Binding& binding(const Host& host) noexcept { return host.m_binding; }

		/** The component an atom names, or NO_COMPONENT. */
		[[nodiscard]] static ecs::ComponentId component_of_atom(const Host& host, int atom) noexcept
		{
			return atom >= 0 && static_cast<size_t>(atom) < host.m_component_by_atom.size()
					   ? host.m_component_by_atom[static_cast<size_t>(atom)]
					   : ecs::NO_COMPONENT;
		}

		/** An entity verb by atom: an engine Verb, or Verb::Count + the index of a game entity method. */
		[[nodiscard]] static i16 verb_of_atom(const Host& host, int atom) noexcept
		{
			return atom >= 0 && static_cast<size_t>(atom) < host.m_verb_by_atom.size()
					   ? host.m_verb_by_atom[static_cast<size_t>(atom)]
					   : static_cast<i16>(Verb::None);
		}
	};

	// --- rights -------------------------------------------------------------------------------------

	/** Whether a script of this context may write (or add, or remove) this component. */
	[[nodiscard]] bool may_write(Context context, const Exposed& exposed) noexcept;

	/** Raises the error that says why a write of this component was refused. Never returns. */
	[[noreturn]] void refuse_write(lua_State* L, Context context, const Exposed& exposed, StringView what);

	/** Raises when the function may not be called from the running context. */
	void check_context(lua_State* L, const Host& host, ContextMask where, const char* name);

	/** A call that needs the world open: inside a system. Raises outside one. */
	[[nodiscard]] ecs::Commands& check_commands(lua_State* L, Host& host, const char* what);

	// --- fields (binding.cpp) -------------------------------------------------------------------------

	/** The field's value, from the component's bytes, onto the stack. */
	void push_field(lua_State* L, const Field& field, const void* bytes);

	/** The value at `index` into the field of the component's bytes; raises on a type or range mismatch. */
	void write_field(lua_State* L, const Exposed& exposed, const Field& field, void* bytes, int index, bool checks);

	/** A table of fields at `table` onto the component's bytes, as e:add(T, {...}) and world:spawn() take. */
	void fill_component(lua_State* L, const Exposed& exposed, int table, void* bytes, bool checks);

	/** The entity a handle names, which must still exist. Raises when it is gone. */
	[[nodiscard]] ecs::Entity live_entity(lua_State* L, ecs::World& world, int index);

	// --- installers ---------------------------------------------------------------------------------

	/** The one metatable every handle shares, and the names typeof() gives them. */
	void install_handles(lua_State* L, Host& host);

	/** The `world` table: query and spawn; and `without`. */
	void install_world(lua_State* L, Host& host);

	/** The Shape userdata, the `shape` library and `layers()`. */
	void install_physics(lua_State* L, Host& host);

	void push_shape(lua_State* L, const physics::Shape& shape);
	[[nodiscard]] const physics::Shape& check_shape(lua_State* L, int index);

	/** The ember.d.luau text for this binding (definitions.cpp). */
	void write_definitions_text(const Binding& binding, String& out);
}
