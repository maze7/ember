# Ember::Ecs, the entity component system: components, prefabs, worlds and the systems that run in
# them, on EnTT, at once on the job system. Games reach EnTT through it. serialize is public because
# Replicated components cross the wire with it; Boost.PFR reads the fields interpolation moves.
ember_add_module(Ecs
	SOURCES
		components.cpp
		prefab.cpp
		world.cpp
	PUBLIC_DEPS
		Ember::Core
		EnTT::EnTT
		serialize::serialize
		Boost::pfr
	PRIVATE_DEPS
		Ember::Jobs
)
