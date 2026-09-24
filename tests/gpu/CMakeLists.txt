# The device under threads. Needs a Vulkan adapter at runtime (lavapipe will do); the suite
# skips itself where there is none, so it is safe to build everywhere.
ember_add_test_suite(ember_gpu_tests gpu
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	device_threads_tests.cpp
)

target_link_libraries(ember_gpu_tests PRIVATE Ember::Gpu)
