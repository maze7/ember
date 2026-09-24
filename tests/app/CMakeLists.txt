ember_add_test_suite(ember_app_tests app
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	frame_ring_tests.cpp
	frame_pipeline_tests.cpp
)

# The pipeline test runs the frame loop's ordering under real jobs.
target_link_libraries(ember_app_tests PRIVATE Ember::Jobs)
