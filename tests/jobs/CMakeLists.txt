ember_add_test_suite(ember_jobs_tests jobs
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	fiber_tests.cpp
	job_system_tests.cpp
)

target_link_libraries(ember_jobs_tests PRIVATE Ember::Jobs)
target_include_directories(ember_jobs_tests PRIVATE "${EMBER_ROOT}/src")
