# Reads run on the IO thread and complete job counters, so the suite drives a real scheduler.
ember_add_test_suite(ember_io_tests io
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	file_tests.cpp
)

target_link_libraries(ember_io_tests PRIVATE Ember::Io Ember::Jobs)
