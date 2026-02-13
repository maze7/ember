cmake_minimum_required(VERSION 3.5)
project(Ember)

# c++ version & compiler flags
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O1")
set(CMAKE_CXX_FLAGS_PROFILE "${CMAKE_CXX_FLAGS_PROFILE} -O2")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

# library configuration options
option(EMBER_USE_SDL3 "Whether Ember should use SDL3 for platform layer" ON)
option(EMBER_PLATFORM_STEAM_DECK "Whether Ember is beig built for Steam Deck" OFF)

add_library(Ember
    STATIC
        src/core/common.h
        src/core/logger.cpp
        src/core/time.cpp
        src/core/uuid.cpp
        src/ecs/scene.cpp
        src/ecs/entity.cpp
        src/platform/window.cpp
        src/platform/application.cpp
        src/input/bindings.cpp
        src/input/controller.cpp
        src/input/input.cpp
        src/input/keyboard.cpp
        src/input/mouse.cpp
        src/input/virtual_axis.cpp
        src/input/virtual_input.cpp
        src/input/virtual_stick.cpp
        src/graphics/color.cpp
        src/graphics/target.cpp
        src/graphics/texture.cpp
        src/graphics/batcher.cpp
        src/graphics/blend_mode.cpp
        src/graphics/render_device.cpp
        src/graphics/imgui_renderer.cpp
        src/assets/asset_watcher.cpp
        src/assets/asset_manager.cpp
        src/assets/asset_collection.cpp
)

# add include directories to Ember library
target_include_directories(Ember
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

if (EMBER_PLATFORM_STEAM_DECK)
	target_compile_definitions(Ember PUBLIC EMBER_PLATFORM_STEAM_DECK)
endif()

if (EMBER_USE_SDL3)
    # SDL3
    set(SDL_STATIC ON CACHE BOOL "Build SDL3 as a static library" FORCE)
    set(SDL_SHARED OFF CACHE BOOL "Disable building SDL3 as shared" FORCE)
    add_subdirectory(lib/SDL)

    # SDL_shadercross
    set(SDLSHADERCROSS_SHARED OFF  CACHE BOOL "Disable shared SDL_shadercross" FORCE)
    set(SDLSHADERCROSS_STATIC ON   CACHE BOOL "Enable static SDL_shadercross" FORCE)
    set(SDLSHADERCROSS_DXC OFF CACHE BOOL "Disable HLSL compilation via DXC" FORCE)
    set(SDLSHADERCROSS_INSTALL OFF CACHE BOOL "Disable installing SDL_shadercross" FORCE)
    set(SDLSHADERCROSS_VENDORED ON CACHE BOOL "Use vendored SPIRV‐Cross" FORCE)
    add_subdirectory(lib/SDL_shadercross)

    # SDL-specific includes
    target_include_directories(Ember
        PRIVATE
            lib/SDL/include
            lib/SDL_shadercross/include
    )

    # SDL-specific sources
    target_sources(Ember
        PRIVATE
            src/graphics/sdl/render_device_sdl.cpp
    )

    # SDL Libraries
    target_link_libraries(Ember PUBLIC SDL3::SDL3-static SDL3_shadercross::SDL3_shadercross-static)
	target_compile_definitions(Ember
		PUBLIC
			EMBER_USE_SDL)
endif()

# Ember uses fmt as a replacement for std::format while compiler support is still spotty.
set(BUILD_SHARED_LIBS OFF)
set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
add_subdirectory(lib/fmt)
target_include_directories(Ember PUBLIC lib/fmt/include)
target_link_libraries(Ember PUBLIC fmt::fmt)

# Ember uses GLM for all 3D math
add_subdirectory(lib/glm)
target_include_directories(Ember PUBLIC lib/glm)
target_link_libraries(Ember PUBLIC glm::glm)
target_compile_definitions(Ember PUBLIC GLM_ENABLE_EXPERIMENTAL)

# ENTT powers the Ember ECS
target_include_directories(Ember PUBLIC lib/entt)

# nlohmann::json is used for json
target_include_directories(Ember PUBLIC lib/json)

# Ember uses stb_image.h to load images from disk
target_include_directories(Ember PUBLIC lib/stb)
target_sources(Ember PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lib/stb/stb_image.cpp")

# Ember uses efsw to watch files for changes
add_subdirectory(lib/efsw)
target_include_directories(Ember PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lib/efsw/include")
target_link_libraries(Ember PUBLIC efsw)

# Ember uses ImGui for debug UIs
target_sources(Ember
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/imgui.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/imgui.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/imgui_demo.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/imgui_draw.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/imgui_widgets.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/imgui_tables.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdlgpu3.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdlgpu3.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdlgpu3_shaders.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdl3.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdl3.cpp"
)
target_include_directories(Ember PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui")

# ImPlot for plotting (depends on ImGui)
target_sources(Ember
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/implot/implot.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/implot/implot_internal.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/implot/implot.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/implot/implot_items.cpp"
)
target_include_directories(Ember PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lib/implot")

# compile definitions for different build configurations
target_compile_definitions(Ember PUBLIC
    $<$<CONFIG:Debug>:EMBER_DEBUG>
    $<$<CONFIG:Profile>:EMBER_PROFILE>
    $<$<CONFIG:Release>:EMBER_RELEASE>
)

# If building as a dependency of another project, export the library and include paths to PARENT_SCOPE
if (NOT PROJECT_IS_TOP_LEVEL)
    set(EMBER_LIBS Ember PARENT_SCOPE)
    set(EMBER_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src PARENT_SCOPE)
endif()
