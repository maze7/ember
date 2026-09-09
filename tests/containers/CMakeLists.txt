ember_add_test_suite(ember_container_tests containers
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	pool_tests.cpp
	range_allocator_tests.cpp
	mpmc_queue_tests.cpp
	dirty_set_tests.cpp
)
