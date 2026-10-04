ember_add_test_suite(ember_physics_tests physics
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	rewind_tests.cpp
)

target_link_libraries(ember_physics_tests PRIVATE Ember::Physics)
