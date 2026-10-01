ember_add_test_suite(ember_render_tests render
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	scene_tests.cpp
	scene_parallel_tests.cpp
	gpu_scene_tests.cpp
	view_tests.cpp
	material_registry_tests.cpp
	features/surface_tests.cpp
	features/lighting_tests.cpp
	features/shadow_tests.cpp
	features/screen_tests.cpp
)

target_link_libraries(ember_render_tests
	PRIVATE
		Ember::Render
		Ember::Jobs
		Ember::ShaderCompiler
)
