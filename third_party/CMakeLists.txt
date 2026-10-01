# Enable profiling if requested
if(EMBER_USE_TRACY)
	# Tracy's own CMake option must also be enabled.
	set(TRACY_ENABLE ON)
	set(TRACY_FIBERS ${EMBER_TRACE_FIBERS})
	add_subdirectory(tracy EXCLUDE_FROM_ALL)

	# Tracy stamps the thread handle inside the client, so the call has to survive optimisation.
	set_target_properties(TracyClient PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endif()

# FMT is used by core for logging and string formatting.
add_subdirectory(fmt EXCLUDE_FROM_ALL)
add_subdirectory(stb EXCLUDE_FROM_ALL)

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

# efsw watches the asset directory for hot reload. Built here as a static library, the way imgui
# is: upstream's CMake wants 3.27 and steers BUILD_SHARED_LIBS, which is not ours to touch.
if(EMBER_ENABLE_ASSETS)
	set(EFSW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/efsw")

	add_library(efsw STATIC
		${EFSW_DIR}/src/efsw/Debug.cpp
		${EFSW_DIR}/src/efsw/DirectorySnapshot.cpp
		${EFSW_DIR}/src/efsw/DirectorySnapshotDiff.cpp
		${EFSW_DIR}/src/efsw/DirWatcherGeneric.cpp
		${EFSW_DIR}/src/efsw/FileInfo.cpp
		${EFSW_DIR}/src/efsw/FileSystem.cpp
		${EFSW_DIR}/src/efsw/FileWatcher.cpp
		${EFSW_DIR}/src/efsw/FileWatcherGeneric.cpp
		${EFSW_DIR}/src/efsw/FileWatcherImpl.cpp
		${EFSW_DIR}/src/efsw/Log.cpp
		${EFSW_DIR}/src/efsw/String.cpp
		${EFSW_DIR}/src/efsw/System.cpp
		${EFSW_DIR}/src/efsw/Watcher.cpp
		${EFSW_DIR}/src/efsw/WatcherGeneric.cpp
	)
	add_library(efsw::efsw ALIAS efsw)

	if(WIN32)
		target_sources(efsw PRIVATE
			${EFSW_DIR}/src/efsw/platform/win/FileSystemImpl.cpp
			${EFSW_DIR}/src/efsw/platform/win/SystemImpl.cpp
			${EFSW_DIR}/src/efsw/FileWatcherWin32.cpp
			${EFSW_DIR}/src/efsw/WatcherWin32.cpp
		)
		target_compile_definitions(efsw PRIVATE _SCL_SECURE_NO_WARNINGS)
	else()
		target_sources(efsw PRIVATE
			${EFSW_DIR}/src/efsw/platform/posix/FileSystemImpl.cpp
			${EFSW_DIR}/src/efsw/platform/posix/SystemImpl.cpp
		)
		find_package(Threads REQUIRED)
		target_link_libraries(efsw PRIVATE Threads::Threads)

		if(APPLE)
			target_sources(efsw PRIVATE
				${EFSW_DIR}/src/efsw/FileWatcherFSEvents.cpp
				${EFSW_DIR}/src/efsw/FileWatcherKqueue.cpp
				${EFSW_DIR}/src/efsw/WatcherFSEvents.cpp
				${EFSW_DIR}/src/efsw/WatcherKqueue.cpp
			)
			target_link_libraries(efsw PRIVATE "-framework CoreFoundation" "-framework CoreServices")
		else()
			target_sources(efsw PRIVATE
				${EFSW_DIR}/src/efsw/FileWatcherInotify.cpp
				${EFSW_DIR}/src/efsw/WatcherInotify.cpp
			)
		endif()
	endif()

	target_include_directories(efsw
		SYSTEM PUBLIC "$<BUILD_INTERFACE:${EFSW_DIR}/include>"
		PRIVATE "${EFSW_DIR}/src"
	)
	target_compile_features(efsw PRIVATE cxx_std_11)
	target_compile_definitions(efsw PRIVATE $<IF:$<CONFIG:Debug>,DEBUG,NDEBUG>)
endif()

add_subdirectory(yyjson)

# Ember::Net's dependencies. serialize (Glenn Fiedler) is the wire format: one header, public,
# because game types describe themselves with its macros. The transport is Valve's networking
# sockets, private to Ember::Net as SDL is to Ember::Platform, as Valve::NetworkingSockets: one of two
# libraries with one API.
#
#   GameNetworkingSockets   open source, built here, static, without its examples, tests and tools.
#                           Needs OpenSSL and protobuf from the system (Fedora: dnf install
#                           openssl-devel protobuf-devel; Windows: vcpkg).
#   SteamNetworkingSockets  EMBER_USE_STEAM: Steam's, in the Steamworks SDK, which adds
#                           Steam's relay network and connections by Steam ID. The SDK comes from
#                           partner.steamgames.com/downloads; STEAMWORKS_SDK is its sdk folder.
if(EMBER_ENABLE_NET)
	add_library(serialize INTERFACE)
	add_library(serialize::serialize ALIAS serialize)
	target_include_directories(serialize SYSTEM INTERFACE
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/serialize>"
	)

	add_library(networking_sockets INTERFACE)
	add_library(Valve::NetworkingSockets ALIAS networking_sockets)

	if(EMBER_USE_STEAM)
		set(STEAMWORKS_SDK "" CACHE PATH "The Steamworks SDK's sdk folder, holding public/ and redistributable_bin/")
		if(NOT EXISTS "${STEAMWORKS_SDK}/public/steam/steam_api.h")
			message(FATAL_ERROR "EMBER_USE_STEAM needs STEAMWORKS_SDK: the Steamworks SDK's sdk folder")
		endif()

		# steam_api is a shared library the game ships beside its executable.
		set(steam_bin "${STEAMWORKS_SDK}/redistributable_bin")
		add_library(steam_api SHARED IMPORTED GLOBAL)

		if(WIN32)
			set_target_properties(steam_api PROPERTIES
				IMPORTED_LOCATION "${steam_bin}/win64/steam_api64.dll"
				IMPORTED_IMPLIB "${steam_bin}/win64/steam_api64.lib"
			)
		elseif(APPLE)
			set_target_properties(steam_api PROPERTIES IMPORTED_LOCATION "${steam_bin}/osx/libsteam_api.dylib")
		elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
			set_target_properties(steam_api PROPERTIES IMPORTED_LOCATION "${steam_bin}/linuxarm64/libsteam_api.so")
		else()
			set_target_properties(steam_api PROPERTIES IMPORTED_LOCATION "${steam_bin}/linux64/libsteam_api.so")
		endif()

		target_include_directories(steam_api SYSTEM INTERFACE "${STEAMWORKS_SDK}/public")
		target_link_libraries(networking_sockets INTERFACE steam_api)
	else()
		set(BUILD_STATIC_LIB ON CACHE BOOL "GameNetworkingSockets as a static library" FORCE)
		set(BUILD_SHARED_LIB OFF CACHE BOOL "No shared GameNetworkingSockets" FORCE)
		set(BUILD_EXAMPLES OFF CACHE BOOL "No GameNetworkingSockets examples" FORCE)
		set(BUILD_TESTS OFF CACHE BOOL "No GameNetworkingSockets tests" FORCE)
		set(BUILD_TOOLS OFF CACHE BOOL "No GameNetworkingSockets tools" FORCE)
		add_subdirectory(GameNetworkingSockets EXCLUDE_FROM_ALL)

		# GNS packs and parses its packets with unaligned loads and stores, which UBSan stops under the
		# asan preset. Its memory errors still show under ASan; its undefined behaviour is not ours.
		if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
			target_compile_options(GameNetworkingSockets_s PRIVATE -fno-sanitize=undefined)
		endif()

		target_link_libraries(networking_sockets INTERFACE GameNetworkingSockets::static)
	endif()
endif()
