#pragma once

#include <ember/containers/span.h>
#include <ember/core/bitmask.h>
#include <ember/core/common.h>
#include <ember/ecs/components.h>
#include <ember/ecs/world.h>
#include <ember/memory/memory.h>
#include <ember/physics/components.h>
#include <ember/script/lua.h>
#include <ember/script/source.h>

#include <boost/pfr.hpp>
#include <glm/ext/vector_int2_sized.hpp>
#include <glm/vec2.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

/**
 * What scripts may touch: the components a game exposes, with their fields read off the struct by
 * Boost.PFR; the methods, libraries, globals, enums and constants a binding pack installs; and the
 * stages a game's schedule has. The host installs all of it when it starts, and the definitions
 * file for the editor is written from the same records, so the two never drift.
 */
namespace ember::script
{
	/** The contexts a function may be called from. A lib's call counts as its caller's context. */
	enum class ContextMask : u8
	{
		None   = 0,
		Sim	   = 1 << 0,
		Server = 1 << 1,
		Client = 1 << 2,
		All	   = Sim | Server | Client,
	};
	EMBER_ENUM_BITWISE_OPS(ContextMask, u8);

	[[nodiscard]] constexpr ContextMask mask_of(Context context) noexcept
	{
		switch (context)
		{
			case Context::Sim:
				return ContextMask::Sim;
			case Context::Server:
				return ContextMask::Server;
			case Context::Client:
				return ContextMask::Client;
			default:
				return ContextMask::None;
		}
	}

	/**
	 * One function a pack installs. The signature is Luau's, as luau-lsp reads it, and the only
	 * place it is written: the definitions file copies it. An entity method's signature leaves out
	 * the entity, which is argument 1 when it is called.
	 */
	struct Function
	{
		const char* name	  = nullptr;
		const char* signature = nullptr; // "(around: Entity, radius: number) -> Entity?"
		lua_CFunction call	  = nullptr;
		ContextMask where	  = ContextMask::All;
		const char* label	  = nullptr; // "fx.spawn" in messages; the binding fills it for a library's
	};

	/** How a component is exposed beyond its fields. */
	struct Exposure
	{
		/** Recomputed every tick by the systems that write it, never carried across ticks, so a sim script may write it too: a hitbox. */
		bool derived = false;
	};

	template <class E> struct Named
	{
		const char* name;
		E value;
	};

	/** A field's C++ type, as the binding converts it. Opaque fields are hidden. */
	enum class FieldKind : u8
	{
		Bool,
		Int,
		Float,
		Vec2,
		Vec2i8,
		Enum,
		Layers,
		Shape,
		Text, // const char*: read-only
		Opaque,
		Count
	};

	struct Field
	{
		String name;
		u32 offset	   = 0;
		u32 size	   = 0;
		FieldKind kind = FieldKind::Opaque;
		bool is_signed = false;
		i16 atom	   = -1; // the host's, once it starts
	};

	/** A method the engine or a pack exposes by hand on a component: component is the bytes, self its entity. */
	using MethodFn = int (*)(lua_State* L, void* component, ecs::Entity self);

	struct Method
	{
		String name;
		String signature;
		MethodFn call = nullptr;
		i16 atom	  = -1;
	};

	/** A component as scripts see it. */
	struct Exposed
	{
		const ecs::ComponentInfo* info = nullptr;
		Vector<Field> fields{&memory::heap(MemoryTag::Scripting)};
		Vector<Method> methods{&memory::heap(MemoryTag::Scripting)};
		String field_list{&memory::heap(MemoryTag::Scripting)}; // "at velocity hold": for the error that names what exists
		i16 atom	 = -1;
		bool derived = false;

		[[nodiscard]] const Field* find(i16 atom) const noexcept;
		[[nodiscard]] const Field* find(StringView name) const noexcept; // for a string whose atom was fixed before the field had one
		[[nodiscard]] const Method* find_method(i16 atom) const noexcept;
	};

	struct Library
	{
		String name;
		Vector<Function> functions{&memory::heap(MemoryTag::Scripting)};
		Vector<String> labels{&memory::heap(MemoryTag::Scripting)}; // "fx.spawn": what the functions' labels point at
	};

	struct Enumeration
	{
		struct Value
		{
			String name;
			f64 value = 0.0;
		};
		String name;
		Vector<Value> values{&memory::heap(MemoryTag::Scripting)};
	};

	struct Constant
	{
		String name;
		f64 value = 0.0;
	};

	struct StageName
	{
		String name;
		u8 index = 0;
	};

	/** A tagged userdata type a pack brings: its metatable's __namecall, and how the definitions name it. */
	struct Userdata
	{
		int tag				   = 0;
		String type_name;
		lua_CFunction namecall = nullptr;
	};

	class Binding final
	{
	public:
		explicit Binding(ecs::World& world) noexcept;

		Binding(const Binding&)			   = delete;
		Binding& operator=(const Binding&) = delete;

		/** A component's fields, through PFR, and its kind, which decides who may write it. */
		template <ecs::Component T> void expose(Exposure how = {}) noexcept;

		/** A method on an exposed component. Fn is int(lua_State*, T&, ecs::Entity). */
		template <ecs::Component T, auto Fn> void method(const char* name, const char* signature) noexcept;

		/** A table of functions: fx.spawn(...). The Function records are copied; their names must last. */
		void library(const char* name, Span<const Function> functions) noexcept;

		/** A method on every entity: e:distance(other). The entity is argument 1. */
		void entity_method(const Function& function) noexcept;

		/** A global function: layers(...). */
		void global(const Function& function) noexcept;

		/** An enum as a table of numbers: Layer.Enemy. */
		template <class E> void enumeration(const char* name, Span<const Named<E>> values) noexcept;

		/** A global number: TILE, TICK_RATE, NO_TICK. */
		void constant(const char* name, f64 value) noexcept;

		/** A stage of the game's schedule, by the name scripts use in system("Act", ...). "Present" is reserved. */
		void stage(const char* name, u8 index) noexcept;

		/** A tagged userdata type: its metatable goes in at start. */
		void userdata(int tag, const char* type_name, lua_CFunction namecall) noexcept;

		/** Luau declarations the generator cannot derive, copied into the definitions file: an extern type a pack brings. */
		void definitions(const char* text) noexcept;

		/** A function on the world table: world:nearest_player(e, radius). The world is argument 1. */
		void world_function(const Function& function) noexcept;

		/**
		 * The game's units, which the declarators tiles(), seconds() and the rest convert with: how many
		 * texels a tile is, and how many ticks a second. Unset, the declarators refuse to run.
		 */
		void units(f64 texels_per_tile, f64 ticks_per_second) noexcept;
		[[nodiscard]] f64 texels_per_tile() const noexcept { return m_texels_per_tile; }
		[[nodiscard]] f64 ticks_per_second() const noexcept { return m_ticks_per_second; }

		[[nodiscard]] const Exposed* exposed(ecs::ComponentId id) const noexcept;
		[[nodiscard]] Span<const Exposed> exposures() const noexcept { return {m_exposed.data(), m_exposed.size()}; }
		[[nodiscard]] Span<const Library> libraries() const noexcept { return {m_libraries.data(), m_libraries.size()}; }
		[[nodiscard]] Span<const Function> entity_methods() const noexcept { return {m_entity_methods.data(), m_entity_methods.size()}; }
		[[nodiscard]] Span<const Function> globals() const noexcept { return {m_globals.data(), m_globals.size()}; }
		[[nodiscard]] Span<const Function> world_functions() const noexcept { return {m_world_functions.data(), m_world_functions.size()}; }
		[[nodiscard]] Span<const Enumeration> enumerations() const noexcept { return {m_enumerations.data(), m_enumerations.size()}; }
		[[nodiscard]] Span<const Constant> constants() const noexcept { return {m_constants.data(), m_constants.size()}; }
		[[nodiscard]] Span<const StageName> stages() const noexcept { return {m_stages.data(), m_stages.size()}; }
		[[nodiscard]] Span<const Userdata> userdata_types() const noexcept { return {m_userdata.data(), m_userdata.size()}; }
		[[nodiscard]] Span<const String> extra_definitions() const noexcept { return {m_definitions.data(), m_definitions.size()}; }
		[[nodiscard]] const StageName* find_stage(StringView name) const noexcept;
		[[nodiscard]] ecs::World& world() const noexcept { return m_world; }

	private:
		friend class Host;

		template <class F> [[nodiscard]] static constexpr FieldKind kind_of() noexcept;
		template <class T, size_t I> void add_field(Exposed& exposed, const T& probe, StringView name) noexcept;
		[[nodiscard]] Exposed& exposed_of(const ecs::ComponentInfo& info) noexcept;

		/** A type declared at run time, its fields from its layout: the host exposes every one the registry has. */
		void expose_dynamic(const ecs::ComponentInfo& info) noexcept;

		ecs::World& m_world;
		Vector<Exposed> m_exposed; // by ComponentId; info null when not exposed
		Vector<Library> m_libraries;
		Vector<Function> m_entity_methods;
		Vector<Function> m_globals;
		Vector<Function> m_world_functions;
		f64 m_texels_per_tile  = 0.0;
		f64 m_ticks_per_second = 0.0;
		Vector<Enumeration> m_enumerations;
		Vector<Constant> m_constants;
		Vector<StageName> m_stages;
		Vector<Userdata> m_userdata;
		Vector<String> m_definitions;
	};

	template <class F> constexpr FieldKind Binding::kind_of() noexcept
	{
		if constexpr (std::is_same_v<F, bool>)
			return FieldKind::Bool;
		else if constexpr (std::is_enum_v<F>)
			return FieldKind::Enum;
		else if constexpr (std::is_integral_v<F>)
			return FieldKind::Int;
		else if constexpr (std::is_floating_point_v<F>)
			return FieldKind::Float;
		else if constexpr (std::is_same_v<F, glm::vec2>)
			return FieldKind::Vec2;
		else if constexpr (std::is_same_v<F, glm::i8vec2>)
			return FieldKind::Vec2i8;
		else if constexpr (std::is_same_v<F, physics::Layers>)
			return FieldKind::Layers;
		else if constexpr (std::is_same_v<F, physics::Shape>)
			return FieldKind::Shape;
		else if constexpr (std::is_same_v<F, const char*>)
			return FieldKind::Text;
		else
			return FieldKind::Opaque;
	}

	template <class T, size_t I> void Binding::add_field(Exposed& exposed, const T& probe, StringView name) noexcept
	{
		using F = std::remove_cvref_t<decltype(boost::pfr::get<I>(probe))>;

		Field field;
		field.name	 = String(name, &memory::heap(MemoryTag::Scripting));
		field.offset = static_cast<u32>(reinterpret_cast<const std::byte*>(&boost::pfr::get<I>(probe)) -
										reinterpret_cast<const std::byte*>(&probe));
		field.size	 = sizeof(F);
		field.kind	 = kind_of<F>();
		if constexpr (std::is_enum_v<F>)
			field.is_signed = std::is_signed_v<std::underlying_type_t<F>>;
		else if constexpr (std::is_arithmetic_v<F>)
			field.is_signed = std::is_signed_v<F>;

		// A field named for a Luau keyword gets an underscore: Invulnerable.until_.
		constexpr StringView KEYWORDS[] = {"and",	"break", "do",	   "else",	 "elseif", "end",	 "false", "for",
										   "function", "if",   "in",	   "local",	 "nil",	   "not",	 "or",	  "repeat",
										   "return",   "then", "true",	   "until",	 "while",  "continue"};
		for (const StringView keyword : KEYWORDS)
			if (keyword == name)
				field.name += '_';

		if (field.kind == FieldKind::Opaque)
			return;

		if (!exposed.field_list.empty())
			exposed.field_list += ' ';
		exposed.field_list += field.name;
		exposed.fields.push_back(std::move(field));
	}

	template <ecs::Component T> void Binding::expose(Exposure how) noexcept
	{
		const ecs::ComponentInfo* info = m_world.components().find<T>();
		EMBER_ASSERT(info != nullptr && "register the component before exposing it");
		if (info == nullptr)
			return;

		Exposed& exposed = exposed_of(*info);
		exposed.derived	 = how.derived;

		if constexpr (!std::is_empty_v<T>)
		{
			constexpr auto NAMES = boost::pfr::names_as_array<T>();
			const T probe{};
			[&]<size_t... Is>(std::index_sequence<Is...>) { (add_field<T, Is>(exposed, probe, NAMES[Is]), ...); }(
				std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
		}
	}

	template <ecs::Component T, auto Fn> void Binding::method(const char* name, const char* signature) noexcept
	{
		static_assert(std::is_invocable_r_v<int, decltype(Fn), lua_State*, T&, ecs::Entity>,
					  "a method is int(lua_State*, T&, ecs::Entity)");

		const ecs::ComponentInfo* info = m_world.components().find<T>();
		EMBER_ASSERT(info != nullptr && "register and expose the component before its methods");
		if (info == nullptr)
			return;

		Method method;
		method.name		 = String(name, &memory::heap(MemoryTag::Scripting));
		method.signature = String(signature, &memory::heap(MemoryTag::Scripting));
		method.call		 = [](lua_State* L, void* component, ecs::Entity self) -> int
		{ return Fn(L, *static_cast<T*>(component), self); };
		exposed_of(*info).methods.push_back(std::move(method));
	}

	template <class E> void Binding::enumeration(const char* name, Span<const Named<E>> values) noexcept
	{
		Enumeration enumeration;
		enumeration.name = String(name, &memory::heap(MemoryTag::Scripting));
		for (const Named<E>& value : values)
			enumeration.values.push_back({.name	 = String(value.name, &memory::heap(MemoryTag::Scripting)),
										  .value = static_cast<f64>(static_cast<std::underlying_type_t<E>>(value.value))});
		m_enumerations.push_back(std::move(enumeration));
	}
}
