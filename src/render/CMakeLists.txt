# Ember::Render is the scene renderer built on the RHI.
ember_add_module(Render
	SOURCES
		graph.cpp
		scene.cpp
		geometry.cpp
	PUBLIC_DEPS
		Ember::Gpu
)
