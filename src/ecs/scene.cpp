#include "ecs/scene.h"
#include "ecs/entity.h"

using namespace Ember;

Entity Scene::create_entity() {
	return {m_registry.create(), this};
}

void Scene::dispose_entity(Entity entity) {
	m_registry.destroy(entity);
}

bool Scene::valid(Entity entity) {
	return m_registry.valid(entity);
}
