# stb_image; the cpp is the implementation TU.
add_library(stb STATIC stb_image.cpp)
add_library(Stb::Stb ALIAS stb)
target_include_directories(stb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
