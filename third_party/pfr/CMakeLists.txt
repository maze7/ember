# Boost.PFR (Antony Polukhin), header only, vendored from game2: a plain struct's fields without
# macros. Ember::Ecs interpolates components with it. Update by replacing include/boost.
add_library(pfr INTERFACE)
add_library(Boost::pfr ALIAS pfr)
target_include_directories(pfr SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(pfr INTERFACE cxx_std_17)
