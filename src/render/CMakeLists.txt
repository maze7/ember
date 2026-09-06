# Ember::Render is the scene renderer built on the RHI.
ember_add_module(Render
	SOURCES
		graph.cpp
		scene.cpp
		geometry.cpp
		gpu_scene.cpp
		visibility.cpp
		material.cpp
		renderer.cpp
		features/mesh.cpp
		features/upscale.cpp
		features/sprite.cpp
	PUBLIC_DEPS
		Ember::Gpu
)

ember_embed_shaders(ember_render
	NAMESPACE ember::render::embedded
	HEADER ember/render/embedded_shaders.h
	SHADERS
		cull.slang
		mesh.slang
		sprite.slang
		upscale.slang
)
