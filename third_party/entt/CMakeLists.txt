# EnTT single header, vendored from game2. Update by replacing include/entt/entt.hpp.
add_library(entt INTERFACE)
add_library(EnTT::EnTT ALIAS entt)
target_include_directories(entt INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(entt INTERFACE cxx_std_20)
