#include "ecs/entity.h"

#include "ecs/scene.h"

using namespace Ember;

Entity::Entity(entt::entity handle, Scene* scene) : m_entity(handle), m_scene(scene) {}
