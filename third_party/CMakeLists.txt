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
