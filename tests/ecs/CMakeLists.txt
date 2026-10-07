ember_add_test_suite(ember_ecs_tests ecs
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	component_tests.cpp
	dynamic_tests.cpp
	prefab_tests.cpp
	schedule_tests.cpp
	world_tests.cpp
)

target_link_libraries(ember_ecs_tests PRIVATE Ember::Ecs Ember::Jobs)
