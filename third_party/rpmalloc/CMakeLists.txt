# ---------------------------------------------------------------------------
# rpmalloc - vendored lock-free thread-caching allocator
#   upstream : https://github.com/mjansson/rpmalloc
#   revision : 262c698d701952553a59d7ef2a8f8ebb4e975572
#   sources  : rpmalloc/rpmalloc.c (includes rpmalloc/malloc.c)
# ---------------------------------------------------------------------------

option(RPMALLOC_ENABLE_OVERRIDE "Replace global malloc/free with rpmalloc" OFF)
option(RPMALLOC_ENABLE_STATISTICS "Collect rpmalloc allocation statistics" OFF)
option(RPMALLOC_ENABLE_ASSERTS "Enable rpmalloc internal assertions" OFF)
option(RPMALLOC_ENABLE_VALIDATE_ARGS "Validate size arguments on every call" OFF)

add_library(rpmalloc STATIC
    rpmalloc/rpmalloc.c
)

add_library(rpmalloc::rpmalloc ALIAS rpmalloc)

target_include_directories(rpmalloc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rpmalloc>
)

target_compile_definitions(rpmalloc
    PRIVATE
        ENABLE_OVERRIDE=$<BOOL:${RPMALLOC_ENABLE_OVERRIDE}>
        ENABLE_STATISTICS=$<BOOL:${RPMALLOC_ENABLE_STATISTICS}>
        ENABLE_ASSERTS=$<BOOL:${RPMALLOC_ENABLE_ASSERTS}>
        ENABLE_VALIDATE_ARGS=$<BOOL:${RPMALLOC_ENABLE_VALIDATE_ARGS}>
)

set_target_properties(rpmalloc PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

if(NOT WIN32)
    target_compile_definitions(rpmalloc PRIVATE _DEFAULT_SOURCE)
endif()

find_package(Threads REQUIRED)
target_link_libraries(rpmalloc PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(rpmalloc PRIVATE /W0)
else()
    target_compile_options(rpmalloc PRIVATE -w)
endif()
