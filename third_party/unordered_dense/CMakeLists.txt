# ---------------------------------------------------------------------------
# unordered_dense - vendored single-header flat hash map
#   upstream : https://github.com/martinus/unordered_dense  (MIT)
#   version  : v4.5.0r
#   header   : include/ankerl/unordered_dense.h
# ---------------------------------------------------------------------------
add_library(unordered_dense INTERFACE)
add_library(unordered_dense::unordered_dense ALIAS unordered_dense)

# SYSTEM so the header is exempt from our strict warnings (-Wconversion/-Wshadow
# /-Wpedantic/-Werror). This is version-portable (works on CMake < 3.25 too).
target_include_directories(unordered_dense SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_features(unordered_dense INTERFACE cxx_std_17)
