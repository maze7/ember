# Ember::App boots the stack and pumps frames. Ember::Main owns the platform
# entry point; games link it, tools keep their own main.
if (NOT EMBER_ENABLE_GRAPHICS OR NOT EMBER_ENABLE_IMGUI)
	message(FATAL_ERROR "EMBER_ENABLE_APP requires EMBER_ENABLE_GRAPHICS and EMBER_ENABLE_IMGUI")
endif()

ember_add_module(App
	SOURCES
		app.cpp
		runtime.cpp
	PUBLIC_DEPS
		Ember::Core
		Ember::Platform
		Ember::Gpu
	PRIVATE_DEPS
		Ember::ImGui
)

if(WIN32)
	set(EMBER_ENTRY_SOURCE main_win32.cpp)
else()
	set(EMBER_ENTRY_SOURCE main_linux.cpp)
endif()

ember_add_module(Main
	SOURCES
		${EMBER_ENTRY_SOURCE}
	PUBLIC_DEPS
		Ember::App
)
