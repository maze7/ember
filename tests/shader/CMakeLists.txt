# The compiler runs on the engine's real shader directory and on material files written into a
# scratch directory, so every test goes through the file paths hot reload and the cooker use. The
# pipeline test drives a device, needs a Vulkan adapter and skips without one.
ember_add_test_suite(ember_shader_tests shader
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	compiler_tests.cpp
	pipeline_tests.cpp
)

target_link_libraries(ember_shader_tests PRIVATE Ember::ShaderCompiler Ember::Gpu)
