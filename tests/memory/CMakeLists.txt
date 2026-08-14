ember_add_test_suite(ember_memory_tests memory
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	memory_tests.cpp
	virtual_memory_tests.cpp
	pmr/arena_resource_tests.cpp
	pmr/heap_resource_tests.cpp
)
