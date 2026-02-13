#pragma once

#include "entt.hpp"

namespace Ember
{
	class Entity;
	class Scene
	{
	public:
		Entity create_entity();
		void dispose_entity(Entity entity);

		// access a view of all entities in the scene with the given components
		template<typename ...Components>
		auto view() { return m_registry.view<Components...>(); }

		template<typename Type, typename Compare>
		void sort(Compare compare) {
			m_registry.sort<Type>(compare);
		}

		/**
		 * @returns True if the Entity ID corresponds to a valid Entity.
		 */
		bool valid(Entity entity);

		// Access the underlying EnTT registry
		[[nodiscard]] auto& registry() { return m_registry; }

		// Access the ENTT context
		[[nodiscard]] decltype(auto) ctx() { return m_registry.ctx(); }
		[[nodiscard]] decltype(auto) ctx() const { return m_registry.ctx(); }

	private:
		friend class Entity;

		entt::registry m_registry;
	};
}
