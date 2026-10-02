#pragma once

#include <ember/containers/span.h>
#include <ember/ecs/component.h>
#include <ember/ecs/wire.h>
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

		void (*assure)(entt::registry& registry) noexcept									 = nullptr;
		void (*emplace)(entt::registry& registry, Entity entity, const void* value) noexcept = nullptr;
		void (*remove)(entt::registry& registry, Entity entity) noexcept					 = nullptr;
		const void* (*find)(const entt::registry& registry, Entity entity) noexcept = nullptr; // null when absent
		void* (*get)(entt::registry& registry, Entity entity) noexcept = nullptr; // to write in place; null when absent

		// A Replicated component's wire form, from its serialize(); null for the rest.
		bool (*write)(serialize::WriteStream& stream, const void* value) noexcept = nullptr;
		bool (*read)(serialize::ReadStream& stream, void* value) noexcept		  = nullptr;

		// An Interpolated component between two samples, from its interpolate() or field by field; null for the rest.
		void (*interpolate)(const void* from, const void* to, f32 t, void* out) noexcept = nullptr;
	};

	namespace detail
	{
		/** Somewhere for find() to point when a tag is present: tags have no bytes of their own. */
		template <class T> inline T TAG_VALUE{};

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

		template <class T> [[nodiscard]] void* get(entt::registry& registry, Entity entity) noexcept
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

			const T empty{};
			info.defaults.resize(sizeof(T));
			std::memcpy(info.defaults.data(), &empty, sizeof(T));

			info.assure	 = [](entt::registry& registry) noexcept { (void)registry.storage<T>(); };
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
}
