ember_add_test_suite(ember_render_tests render
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	scene_tests.cpp
	gpu_scene_tests.cpp
	view_tests.cpp
)

# Scene storage builds into Ember::Render; the suite defaults only link Core.
target_link_libraries(ember_render_tests PRIVATE Ember::Render)
