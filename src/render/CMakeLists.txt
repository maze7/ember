# Ember::Render is the scene renderer built on the RHI.
ember_add_module(Render
	SOURCES
		graph.cpp
		scene.cpp
		geometry.cpp
		gpu_scene.cpp
		visibility.cpp
		material_registry.cpp
		renderer.cpp
		features/surface.cpp
		features/upscale.cpp
		features/lighting.cpp
	PUBLIC_DEPS
		Ember::Gpu
		Ember::Material
)

ember_embed_shaders(ember_render
	NAMESPACE ember::render::embedded
	HEADER ember/render/embedded_shaders.h
	SHADERS
		cull.slang
		upscale.slang
	MATERIALS
		materials/error.slang
		materials/unlit.slang
		materials/sprite.slang
)
