#pragma once

#include "core/common.h"
#include "core/uuid.h"
#include "ecs/scene.h"

namespace Ember
{
	class Entity
	{
	public:
		// Constructors
		Entity() = default;
		Entity(entt::entity handle, Scene* scene);
		Entity(const Entity& other) = default;

		// Add a component to the Entity
		template <class Component, class... Args>
		Component& add(Args&&... args) {
			EMBER_ASSERT(!has<Component>());
			Component& component = m_scene->m_registry.emplace<Component>(m_entity, std::forward<Args>(args)...);
			return component;
		}

		// Access an existing component on the Entity
		template <class Component>
		Component& get() {
			EMBER_ASSERT(has<Component>());
			return m_scene->m_registry.get<Component>(m_entity);
		}

		// Attempt to access an existing component on the Entity via pointer
		template <class Component>
		Component* try_get() {
			return m_scene->m_registry.try_get<Component>(m_entity);
		}

		// Check whether the entity has a component of the given type
		template <class Component>
		bool has() const {
			return m_scene->m_registry.all_of<Component>(m_entity);
		}

		// Remove a component from the Entity
		template <class Component>
		void remove() {
			EMBER_ASSERT(has<Component>());
			m_scene->m_registry.remove<Component>(m_entity);
		}

		void destroy() {
			m_scene->m_registry.destroy(m_entity);
		}

		[[nodiscard]] bool is_null() {
			return (m_scene == nullptr || m_entity == entt::null);
		}

		// Determine if the entity is a balid handle
		operator bool() const { return m_entity != entt::null; }

		// Implicitly convert the Entity to an entt::entity
		operator entt::entity() const { return m_entity; }

		// Implictly convert the Entity to a u32 identifier
		operator u32() const { return (u32) m_entity; }

		// Determine if another Entity handle points to the same entity within the same scene
		bool operator==(const Entity& other) const {
			return m_entity == other.m_entity && m_scene == other.m_scene;
		}

		// Determine if the given Entity is different to another Entity
		bool operator!=(const Entity& other) const {
			return !(*this == other);
		}

	private:
		Scene* m_scene = nullptr;
		entt::entity m_entity{ entt::null };
	};
}
