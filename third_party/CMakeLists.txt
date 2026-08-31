# Enable profiling if requested
if(EMBER_USE_TRACY)
	# Tracy's own CMake option must also be enabled.
	set(TRACY_ENABLE ON)
	add_subdirectory(tracy EXCLUDE_FROM_ALL)
endif()

# FMT is used by core for logging and string formatting.
add_subdirectory(fmt EXCLUDE_FROM_ALL)

# ankerl::undordered_dense is used as an efficient
# substitute for std::unordered_map
add_subdirectory(unordered_dense EXCLUDE_FROM_ALL)

# rpmalloc is used as the general heap on platforms that support it
if (EMBER_USE_RPMALLOC)
	add_subdirectory(rpmalloc EXCLUDE_FROM_ALL)
endif()

# GoogleTest is only configured when tests are, and never installed/shipped.
if(EMBER_BUILD_TESTS)
	set(BUILD_GMOCK OFF)
	set(INSTALL_GTEST OFF)
	add_subdirectory(googletest EXCLUDE_FROM_ALL)
endif()

# tl::expected is used as an alternative to exceptions
add_library(tl_expected INTERFACE)
add_library(tl::expected ALIAS tl_expected)
target_include_directories(tl_expected SYSTEM INTERFACE
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/expected/include>"
)

# GLM is used for all lin-alg
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "Use header-only GLM" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "Disable GLM tests" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "Disable GLM installation" FORCE)
add_subdirectory(glm EXCLUDE_FROM_ALL)

# SDL3 is used for input/windowing on most platforms

# SDL is a private implementation detail of Ember::Platform.
if(EMBER_ENABLE_PLATFORM)
	if(NOT EMBER_USE_SDL)
		message(FATAL_ERROR
			"EMBER_ENABLE_PLATFORM currently requires EMBER_USE_SDL")
	endif()

	set(SDL_SHARED OFF CACHE BOOL "Disable shared SDL" FORCE)
	set(SDL_STATIC ON CACHE BOOL "Build static SDL" FORCE)

	set(SDL_VIDEO ON CACHE BOOL "Required by Ember::Platform" FORCE)
	set(SDL_JOYSTICK ON CACHE BOOL "Required by Ember::Platform" FORCE)

	set(SDL_TEST_LIBRARY OFF CACHE BOOL "Disable SDL test library" FORCE)
	set(SDL_TESTS OFF CACHE BOOL "Disable SDL tests" FORCE)
	set(SDL_EXAMPLES OFF CACHE BOOL "Disable SDL examples" FORCE)
	set(SDL_INSTALL OFF CACHE BOOL "Disable SDL installation" FORCE)

	add_subdirectory(SDL EXCLUDE_FROM_ALL)
endif()

# Vulkan backend dependencies.
if (EMBER_USE_VULKAN)
	add_subdirectory(volk EXCLUDE_FROM_ALL)

	add_library(vma INTERFACE)
	add_library(vma::vma ALIAS vma)
	target_include_directories(vma SYSTEM INTERFACE
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vma/include>"
	)

	target_compile_definitions(vma INTERFACE
		VMA_STATIC_VULKAN_FUNCTIONS=0
		VMA_DYNAMIC_VULKAN_FUNCTIONS=1
	)

	target_link_libraries(vma INTERFACE volk::volk_headers)
endif()

# Dear ImGui backs Ember::ImGui. Compiled here because upstream ships no build files.
if(EMBER_ENABLE_IMGUI AND EMBER_ENABLE_GRAPHICS)
	add_library(imgui STATIC
		imgui/imgui.cpp
		imgui/imgui_demo.cpp
		imgui/imgui_draw.cpp
		imgui/imgui_tables.cpp
		imgui/imgui_widgets.cpp
	)
	target_include_directories(imgui SYSTEM PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/imgui>"
	)
	add_library(imgui::imgui ALIAS imgui)
endif()
