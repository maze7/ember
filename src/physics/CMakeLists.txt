# Ember::Physics, what collides: shapes, the space that sorts them, moves that stop at walls, and the
# hits between hitboxes and hurtboxes. Headless: a dedicated server links it.
ember_add_module(Physics
	SOURCES
		shape.cpp
		space.cpp
		systems.cpp
	PUBLIC_DEPS
		Ember::Core
		Ember::Ecs
)
