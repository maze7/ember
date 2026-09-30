# ember_cook, the build's shader compiler: every ember_cook_shader and ember_cook_material runs it.
# It links the compiler and nothing of the GPU backend, so an edit to the backend neither relinks it
# nor recooks a shader.
add_executable(ember_cook main.cpp)
target_link_libraries(ember_cook PRIVATE Ember::ShaderCompiler ember_build_options)
