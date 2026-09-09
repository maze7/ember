ember_add_test_suite(ember_memory_tests memory
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	memory_tests.cpp
	virtual_memory_tests.cpp
	tagged_heap_tests.cpp
	frame_memory_tests.cpp
	pmr/arena_resource_tests.cpp
	pmr/heap_resource_tests.cpp
)

target_link_libraries(ember_memory_tests PRIVATE Ember::Jobs)
