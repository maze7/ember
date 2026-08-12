cmake_minimum_required(VERSION 3.20)
project(Ember VERSION 0.1.0 LANGUAGES CXX)

# Library configuration options
option(EMBER_ENABLE_PLATFORM "Build platform support" OFF)
option(EMBER_ENABLE_GRAPHICS "Build graphics support" OFF)
option(EMBER_ENABLE_ASSETS   "Build asset system" OFF)
option(EMBER_ENABLE_AUDIO    "Build audio support" OFF)
option(EMBER_USE_TRACY    	 "Enable Tracy profiling" OFF)
option(EMBER_BUILD_TESTS 		 "Build Ember tests" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(EMBER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

list(APPEND CMAKE_MODULE_PATH "${EMBER_ROOT}/cmake")
include(EmberModule)

# Enable profiling if requested
if(EMBER_USE_TRACY)
	# Tracy's own CMake option must also be enabled.
	set(TRACY_ENABLE ON)
	add_subdirectory(third_party/tracy EXCLUDE_FROM_ALL)
endif()

# FMT is used by core for logging and string formatting.
if(NOT TARGET fmt::fmt)
	add_subdirectory(third_party/fmt EXCLUDE_FROM_ALL)
endif()

add_subdirectory(src)

if(EMBER_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
