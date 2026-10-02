#pragma once

#include <ember/containers/span.h>
#include <ember/ecs/component.h>
#include <ember/memory/memory.h>

#include <entt/entt.hpp>

#include <cstring>
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

	/** A field a prefab file can set. */
	struct FieldInfo
	{
		std::string_view name;
		bool (*read)(void* component, JsonValue json) noexcept = nullptr; // false leaves the component alone
	};

	/**
	 * A component type at run time: what prefabs, tools and the net layer work with when they hold bytes
	 * rather than C++ type.
	 */
	struct ComponentInfo
	{
		std::string_view name;
		Kind kind		   = Kind::None;
		u32 size		   = 0;
		ComponentId id	   = 0;
		entt::id_type type = 0; // the C++ type, as EnTT names it

		Vector<u8> defaults; // T{}
		Vector<FieldInfo> fields;

		void (*assure)(entt::registry& registry) noexcept									 = nullptr;
		void (*emplace)(entt::registry& registry, Entity entity, const void* value) noexcept = nullptr;
		void (*remove)(entt::registry& registry, Entity entity) noexcept					 = nullptr;
		const void* (*find)(const entt::registry& registry, Entity entity) noexcept = nullptr; // null when absent
	};

	namespace detail
	{
		/** Somewhere for find() to point when a tag is present: tags have no bytes of their own. */
		template <class T> inline const T TAG_VALUE{};

		/** Gives an entity a component form its byte, replacing one it has. */
		template <class T> void emplace(entt::registry& registry, Entity entity, const void* value) noexcept
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

		template <class T> void remove(entt::registry& registry, Entity entity) noexcept { registry.remove<T>(entity); }

		template <class T> [[nodiscard]] const void* find(const entt::registry& registry, Entity entity) noexcept
		{
			if constexpr (std::is_empty_v<T>)
				return registry.all_of<T>(entity) ? &TAG_VALUE<T> : nullptr;
			else
				return registry.try_get<T>(entity);
		}

		template <class T, size_t I> [[nodiscard]] bool read_field(void* bytes, JsonValue json) noexcept
		{
			constexpr auto field = std::get<I>(description_of<T>.fields);
			T component;
			std::memcpy(&component, bytes, sizeof(T));
			if (!read_value(component.*(field.member), json))
				return false;

			std::memcpy(bytes, &component, sizeof(T));
			return true;
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

			const T empty{};
			info.defaults.resize(sizeof(T));
			std::memcpy(info.defaults.data(), &empty, sizeof(T));

			add_fields<T>(info, std::make_index_sequence<std::tuple_size_v<decltype(description_of<T>.fields)>>{});

			info.assure	 = [](entt::registry& registry) noexcept { (void)registry.storage<T>(); };
			info.emplace = &detail::emplace<T>;
			info.remove	 = &detail::remove<T>;
			info.find	 = &detail::find<T>;
			return info.id;
		}

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
		template <class T, size_t... Is> static void add_fields(ComponentInfo& info, std::index_sequence<Is...>)
		{
			(info.fields.push_back({std::get<Is>(description_of<T>.fields).name, &detail::read_field<T, Is>}), ...);
		}
		Vector<ComponentInfo> m_infos{&memory::heap(MemoryTag::ECS)};
	};
}
