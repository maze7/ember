cmake_minimum_required(VERSION 3.20)
project(Ember VERSION 0.1.0 LANGUAGES C CXX)

# Library configuration options
option(EMBER_ENABLE_PLATFORM "Build platform support" ON)
option(EMBER_ENABLE_GRAPHICS "Build graphics support" ON)
option(EMBER_ENABLE_ASSETS   "Build asset system" OFF)
option(EMBER_ENABLE_AUDIO    "Build audio support" OFF)
option(EMBER_USE_TRACY    	 "Enable Tracy profiling" OFF)
option(EMBER_USE_RPMALLOC 	 "Rely on rpmalloc as the general heap" ON)
option(EMBER_USE_SDL 				 "Rely on SDL3 for input, windowing, platform abstraction" ON)
option(EMBER_USE_VULKAN 		 "Rely on Vulkan for the GPU backend" ON)
option(EMBER_BUILD_TESTS 		 "Build Ember tests" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(EMBER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

list(APPEND CMAKE_MODULE_PATH "${EMBER_ROOT}/cmake")
include(EmberModule)

add_subdirectory(third_party)
add_subdirectory(src)

if(EMBER_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
