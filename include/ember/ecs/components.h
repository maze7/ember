#pragma once

#include <ember/containers/span.h>
#include <ember/ecs/component.h>
#include <ember/ecs/wire.h>
#include <ember/memory/memory.h>

#include <entt/entt.hpp>

#include <cstring>
#include <new>
#include <utility>

namespace ember::ecs
{
	/** An entity in a world: EnTT's own handle, an index and a generation */
	using Entity = entt::entity;

	inline constexpr Entity NO_ENTITY = entt::null;

	/** A component type's place in a Components: the order the game registered it in. */
	using ComponentId = u16;

	inline constexpr ComponentId NO_COMPONENT = 0xffff;

	/** Component types a game has at most. */
	inline constexpr u32 MAX_COMPONENTS = 1024;

	/** The types a field of a component declared at run time may have: what a script's `component` offers. */
	enum class FieldType : u8
	{
		Bool,
		U8,
		U16,
		U32,
		I32,
		F32,
		Vec2,
		Entity, // another entity, by its network id (u32): 0 for none. The script host resolves it
		Name,	// a name as a 64 bit text hash: a clip, a prefab, a sound; 0 for none
		Count
	};

	[[nodiscard]] constexpr u32 field_size(FieldType type) noexcept
	{
		switch (type)
		{
			case FieldType::Bool:
			case FieldType::U8:
				return 1;
			case FieldType::U16:
				return 2;
			case FieldType::U32:
			case FieldType::I32:
			case FieldType::F32:
			case FieldType::Entity:
				return 4;
			case FieldType::Vec2:
			case FieldType::Name:
				return 8;
			default:
				return 0;
		}
	}

	/** One field of a component declared at run time: where it lies in the component's bytes. */
	struct FieldInfo
	{
		String name;
		FieldType type = FieldType::F32;
		u32 offset	   = 0;
	};

	/** A field as a declaration gives it: its name, type and starting value, which is a number for all but a vector. */
	struct FieldDef
	{
		String name;
		FieldType type = FieldType::F32;
		f64 value	   = 0.0; // Bool: 0 or 1; the number for the rest
		f32 y		   = 0.0f; // a Vec2's second component; `value` is its first
		u64 bits	   = 0;	   // a Name's hash, which a double cannot hold
	};

	/**
	 * A component type declared at run time, by a script or a tool, rather than by EMBER_COMPONENT: a
	 * name, a kind and plain fields. Registered through Registry::add_component().
	 */
	struct DynamicComponentDef
	{
		String name;
		Kind kind = Kind::Sim;
		Vector<FieldDef> fields;
	};

	/**
	 * A component type at run time: what prefabs, tools and the net layer work with when they hold bytes
	 * rather than C++ type. Every operation takes the info itself first, so a type declared at run time,
	 * whose bytes live in a storage named after it, can find its own.
	 */
	struct ComponentInfo
	{
		std::string_view name;
		Kind kind		   = Kind::None;
		u32 size		   = 0;
		ComponentId id	   = 0;
		entt::id_type type = 0; // the C++ type, as EnTT names it; a dynamic type's own name
		bool dynamic	   = false;

		Vector<u8> defaults;	   // T{}, or the declaration's starting values
		Vector<FieldInfo> fields;  // a dynamic type's layout; empty for a C++ type, whose fields Boost.PFR reads
		String own_name;		   // a dynamic type's name, which `name` points into

		void (*assure)(const ComponentInfo& self, entt::registry& registry) noexcept = nullptr;
		void (*emplace)(const ComponentInfo& self, entt::registry& registry, Entity entity, const void* value) noexcept =
			nullptr;
		void (*remove)(const ComponentInfo& self, entt::registry& registry, Entity entity) noexcept = nullptr;
		const void* (*find)(const ComponentInfo& self, const entt::registry& registry,
							Entity entity) noexcept = nullptr; // null when absent
		void* (*get)(const ComponentInfo& self, entt::registry& registry,
					 Entity entity) noexcept = nullptr; // to write in place; null when absent

		// A Replicated component's wire form, from its serialize(); null for the rest.
		bool (*write)(const ComponentInfo& self, serialize::WriteStream& stream, const void* value) noexcept = nullptr;
		bool (*read)(const ComponentInfo& self, serialize::ReadStream& stream, void* value) noexcept		   = nullptr;

		// An Interpolated component between two samples, from its interpolate() or field by field; null for the rest.
		void (*interpolate)(const ComponentInfo& self, const void* from, const void* to, f32 t, void* out) noexcept =
			nullptr;

		/** A dynamic type's field by name; null for a C++ type or a name it lacks. */
		[[nodiscard]] const FieldInfo* field(StringView name) const noexcept;
	};

	namespace detail
	{
		/** Somewhere for find() to point when a tag is present: tags have no bytes of their own. */
		template <class T> inline T TAG_VALUE{};

		/** Gives an entity a component form its byte, replacing one it has. */
		template <class T>
		void emplace(const ComponentInfo&, entt::registry& registry, Entity entity, const void* value) noexcept
		{
			if constexpr (std::is_empty_v<T>)
			{
				(void)value;
				registry.emplace_or_replace<T>(entity);
			}
			else
			{
				T copy;
				std::memcpy(&copy, value, sizeof(T));
				registry.emplace_or_replace<T>(entity, copy);
			}
		}

		template <class T> void remove(const ComponentInfo&, entt::registry& registry, Entity entity) noexcept
		{
			registry.remove<T>(entity);
		}

		template <class T>
		[[nodiscard]] const void* find(const ComponentInfo&, const entt::registry& registry, Entity entity) noexcept
		{
			if constexpr (std::is_empty_v<T>)
				return registry.all_of<T>(entity) ? &TAG_VALUE<T> : nullptr;
			else
				return registry.try_get<T>(entity);
		}

		template <class T> [[nodiscard]] void* get(const ComponentInfo&, entt::registry& registry, Entity entity) noexcept
		{
			if constexpr (std::is_empty_v<T>)
				return registry.all_of<T>(entity) ? &TAG_VALUE<T> : nullptr;
			else
				return registry.try_get<T>(entity);
		}
	}

	/**
	 * Every component type a game has, numbered in the order the game registered them, which is the
	 * same on every machine that runs the same build. An info stays where it is for as long as the
	 * Components lives, so pointers to it may be kept.
	 */
	class Components final
	{
	public:
		Components() noexcept { m_infos.reserve(MAX_COMPONENTS); }

		Components(const Components&)			 = delete;
		Components& operator=(const Components&) = delete;

		/** Registers T, once; its id either way. */
		template <Component T> ComponentId add() noexcept
		{
			const entt::id_type type = entt::type_hash<T>::value();
			if (const ComponentInfo* existing = find(type); existing != nullptr)
				return existing->id;

			EMBER_ASSERT(m_infos.size() < MAX_COMPONENTS && "more component types than MAX_COMPONENTS");

			ComponentInfo& info = m_infos.emplace_back();
			info.name			= description_of<T>.name;
			info.kind			= kind_of<T>;
			info.size			= sizeof(T);
			info.id				= static_cast<ComponentId>(m_infos.size() - 1);
			info.type			= type;

			// T{} over zeroed bytes: the constructor writes the fields and leaves the padding as it found it, so
			// two machines agree on every byte of the defaults, which prefabs, the wire and the fingerprint compare.
			info.defaults.assign(sizeof(T), 0);
			new (info.defaults.data()) T{};

			info.assure	 = [](const ComponentInfo&, entt::registry& registry) noexcept { (void)registry.storage<T>(); };
			info.emplace = &detail::emplace<T>;
			info.remove	 = &detail::remove<T>;
			info.find	 = &detail::find<T>;
			info.get	 = &detail::get<T>;

			if constexpr (ReplicatedComponent<T>)
			{
				static_assert(
					Serializable<T> || std::is_empty_v<T>,
					"a Replicated component crosses the wire with template <class Stream> bool serialize(Stream&)");
				static_assert(sizeof(T) <= MAX_REPLICATED_BYTES,
							  "a Replicated component is 64 bytes at most: split it");
				info.write = &detail::write_component<T>;
				info.read  = &detail::read_component<T>;
			}

			if constexpr (has_any(kind_of<T>, Kind::Interpolated))
			{
				static_assert(OwnInterpolate<T> || std::is_aggregate_v<T>,
							  "an Interpolated component is a plain struct, or has static T interpolate(from, to, t)");
				info.interpolate = &detail::interpolate_component<T>;
			}

			return info.id;
		}

		/**
		 * Registers a type declared at run time, laid out from its fields: each at its natural alignment,
		 * in the order given. Its bytes live in a storage named after it. A name already taken, a kind
		 * that is not one home, or a Replicated type past MAX_REPLICATED_BYTES is refused: NO_COMPONENT.
		 */
		ComponentId add_dynamic(const DynamicComponentDef& def) noexcept;

		/** A type by its name, as EMBER_COMPONENT spelt it; null when there is none. */
		[[nodiscard]] const ComponentInfo* find(std::string_view name) const noexcept;

		/** A type by EnTT's name for it; null when it was never registered. */
		[[nodiscard]] const ComponentInfo* find(entt::id_type type) const noexcept;

		template <Component T> [[nodiscard]] const ComponentInfo* find() const noexcept
		{
			return find(entt::type_hash<T>::value());
		}

		[[nodiscard]] const ComponentInfo& operator[](ComponentId id) const noexcept { return m_infos[id]; }
		[[nodiscard]] Span<const ComponentInfo> all() const noexcept
		{
			return Span<const ComponentInfo>(m_infos.data(), m_infos.size());
		}
		[[nodiscard]] u32 count() const noexcept { return static_cast<u32>(m_infos.size()); }

	private:
		Vector<ComponentInfo> m_infos{&memory::heap(MemoryTag::ECS)};
	};

	/**
	 * A dynamic type's description without registering it: its layout, size, defaults and fields, as
	 * add_dynamic() would make them, for code that converts values before the type exists. False, with
	 * nothing filled, for a definition add_dynamic() would refuse.
	 */
	[[nodiscard]] bool describe_dynamic(const DynamicComponentDef& def, ComponentInfo& out) noexcept;

	/** A dynamic field's value as a number: a bool as 0 or 1, a vector's first component. */
	[[nodiscard]] f64 read_field(const FieldInfo& field, const void* component) noexcept;

	/** A dynamic field's value from a number (and a vector's second component); held to the field's range. */
	void write_field(const FieldInfo& field, void* component, f64 value, f32 y = 0.0f) noexcept;

	/** An Entity or Name field's bits, which a double cannot hold whole: the id, or the hash. */
	[[nodiscard]] u64 read_field_bits(const FieldInfo& field, const void* component) noexcept;
	void write_field_bits(const FieldInfo& field, void* component, u64 bits) noexcept;
}

namespace ember
{
	EMBER_ENUM_NAMES(ecs::FieldType, "Bool", "U8", "U16", "U32", "I32", "F32", "Vec2", "Entity", "Name");
}
