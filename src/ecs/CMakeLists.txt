# Ember::Ecs, the entity component system: components, prefabs, worlds and the systems that run in
# them, on EnTT, at once on the job system. Games reach EnTT through it.
ember_add_module(Ecs
	SOURCES
		components.cpp
		prefab.cpp
		world.cpp
	PUBLIC_DEPS
		Ember::Core
		EnTT::EnTT
	PRIVATE_DEPS
		Ember::Jobs
)
