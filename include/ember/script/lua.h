#pragma once

#include <ember/ecs/components.h>
#include <ember/ecs/world.h>

#include <glm/vec2.hpp>
#include <lua.h>
#include <lualib.h>

/**
 * The Luau VM headers, for a binding pack that writes C functions, and the handles scripts hold.
 * A pack needs nothing else of Luau: these helpers push and read the engine's values, and the host
 * answers for the world a state belongs to.
 */
namespace ember::script
{
	class Host;

	/** What a lightuserdata or userdata is. A game's own tags start at TAG_GAME. */
	enum Tag : int
	{
		TAG_NONE		   = 0,
		TAG_ENTITY		   = 1, // lightuserdata: an entity
		TAG_COMPONENT	   = 2, // lightuserdata: one entity's component, reached as e.Position
		TAG_COMPONENT_TYPE = 3, // lightuserdata: a component type, the global Position
		TAG_FILTER		   = 4, // userdata: without(...)
		TAG_QUERY		   = 5, // userdata: a query under iteration
		TAG_SHAPE		   = 6, // userdata: a physics::Shape
		TAG_GAME		   = 16,
	};

	/** Every lightuserdata handle sets the top bit, so none is null, and the tag alone tells them apart. */
	inline constexpr u64 HANDLE_HIGH = u64{1} << 63;

	/** The host behind a state, and its world: for a pack's functions. */
	[[nodiscard]] Host& host_of(lua_State* L) noexcept;
	[[nodiscard]] ecs::World& world_of(lua_State* L) noexcept;

	void push_entity(lua_State* L, ecs::Entity entity) noexcept;
	[[nodiscard]] ecs::Entity to_entity(lua_State* L, int index) noexcept; // NO_ENTITY when it is not one
	[[nodiscard]] ecs::Entity check_entity(lua_State* L, int index);	   // raises when it is not one

	void push_component(lua_State* L, ecs::Entity entity, ecs::ComponentId component) noexcept;
	[[nodiscard]] bool to_component(lua_State* L, int index, ecs::Entity& entity, ecs::ComponentId& component) noexcept;

	void push_component_type(lua_State* L, ecs::ComponentId component) noexcept;
	[[nodiscard]] ecs::ComponentId to_component_type(lua_State* L, int index) noexcept; // NO_COMPONENT when not one
	[[nodiscard]] ecs::ComponentId check_component_type(lua_State* L, int index);

	/** glm vectors as Luau's native vector, z at zero. */
	void push_vec2(lua_State* L, glm::vec2 value) noexcept;
	[[nodiscard]] glm::vec2 check_vec2(lua_State* L, int index);
	[[nodiscard]] glm::vec2 opt_vec2(lua_State* L, int index, glm::vec2 fallback);
}
