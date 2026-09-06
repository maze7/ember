# Ember::ImGui: Dear ImGui fed by ember input, drawn through ember::gpu.
if (NOT EMBER_ENABLE_GRAPHICS)
	message(FATAL_ERROR "EMBER_ENABLE_IMGUI requires EMBER_ENABLE_GRAPHICS")
endif()

ember_add_module(ImGui
	SOURCES
		imgui_backend.cpp
	PUBLIC_DEPS
		Ember::Core
		Ember::Platform
		Ember::Gpu
		imgui::imgui
)

ember_embed_shaders(ember_imgui
	NAMESPACE ember::imgui::embedded
	HEADER ember/imgui/embedded_shader.h
	SHADERS
		imgui.slang
)
