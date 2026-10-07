#include "internal.h"

#include <cstring>

/**
 * Shapes as scripts hold them: a Shape userdata, made by shape.box() and shape.circle(), turned and
 * placed by its methods, read field by field; and layers(), which ors a game's Layer values into the
 * number a Layers field takes.
 */
namespace ember::script
{
	namespace
	{
		int shape_box(lua_State* L)
		{
			const glm::vec2 size   = check_vec2(L, 1);
			const glm::vec2 center = opt_vec2(L, 2, {});
			push_shape(L, physics::box(size, center));
			return 1;
		}

		int shape_circle(lua_State* L)
		{
			const f32 radius	   = static_cast<f32>(luaL_checknumber(L, 1));
			const glm::vec2 center = opt_vec2(L, 2, {});
			push_shape(L, physics::circle(radius, center));
			return 1;
		}

		int shape_index(lua_State* L)
		{
			const physics::Shape& shape = check_shape(L, 1);
			const StringView key		= luaL_checkstring(L, 2);

			if (key == "center")
				push_vec2(L, shape.center);
			else if (key == "half")
				push_vec2(L, shape.half);
			else if (key == "axis")
				push_vec2(L, shape.axis);
			else if (key == "kind")
				lua_pushnumber(L, static_cast<f64>(shape.kind));
			else
				luaL_error(L, "a Shape has center, half, axis and kind; not '%s'", key.data());
			return 1;
		}

		int shape_namecall(lua_State* L)
		{
			const physics::Shape& shape = check_shape(L, 1);
			const StringView method		= lua_namecallatom(L, nullptr);

			if (method == "turned")
				push_shape(L, shape.turned(check_vec2(L, 2)));
			else if (method == "at")
				push_shape(L, shape.at(check_vec2(L, 2)));
			else
				luaL_error(L, "a Shape has turned() and at(); not '%s'", method.data());
			return 1;
		}

		int shape_tostring(lua_State* L)
		{
			const physics::Shape& shape = check_shape(L, 1);
			if (shape.kind == physics::ShapeKind::Circle)
				lua_pushfstring(L, "circle(%g at %g, %g)", shape.half.x, shape.center.x, shape.center.y);
			else
				lua_pushfstring(L, "box(%g x %g at %g, %g)", shape.half.x * 2.0f, shape.half.y * 2.0f, shape.center.x,
								shape.center.y);
			return 1;
		}

		/** layers(Layer.Player, Layer.Enemy): the bits a Layers field holds. */
		int layers(lua_State* L)
		{
			u32 bits		= 0;
			const int count = lua_gettop(L);
			for (int index = 1; index <= count; ++index)
			{
				const f64 value = luaL_checknumber(L, index);
				if (value < 0.0 || value > 4294967295.0 || static_cast<f64>(static_cast<u32>(value)) != value)
					luaL_error(L, "layers() takes layer bits, not %g", value);
				bits |= static_cast<u32>(value);
			}
			lua_pushnumber(L, static_cast<f64>(bits));
			return 1;
		}
	}

	void push_shape(lua_State* L, const physics::Shape& shape)
	{
		void* bytes = lua_newuserdatataggedwithmetatable(L, sizeof(physics::Shape), TAG_SHAPE);
		std::memcpy(bytes, &shape, sizeof(physics::Shape));
	}

	const physics::Shape& check_shape(lua_State* L, int index)
	{
		const void* bytes = lua_touserdatatagged(L, index, TAG_SHAPE);
		if (bytes == nullptr)
			luaL_typeerror(L, index, "Shape");
		return *static_cast<const physics::Shape*>(bytes);
	}

	void install_physics(lua_State* L, Host& host)
	{
		(void)host;

		lua_newtable(L);
		lua_pushstring(L, "Shape");
		lua_setfield(L, -2, "__type");
		lua_pushcfunction(L, shape_index, "__index");
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, shape_namecall, "__namecall");
		lua_setfield(L, -2, "__namecall");
		lua_pushcfunction(L, shape_tostring, "__tostring");
		lua_setfield(L, -2, "__tostring");
		lua_setreadonly(L, -1, true);
		lua_setuserdatametatable(L, TAG_SHAPE);

		lua_newtable(L);
		lua_pushcfunction(L, shape_box, "shape.box");
		lua_setfield(L, -2, "box");
		lua_pushcfunction(L, shape_circle, "shape.circle");
		lua_setfield(L, -2, "circle");
		lua_setglobal(L, "shape");

		lua_pushcfunction(L, layers, "layers");
		lua_setglobal(L, "layers");
	}
}
