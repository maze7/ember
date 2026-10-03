ember_add_test_suite(ember_anim_tests anim
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	parse_tests.cpp
	sample_tests.cpp
)

target_link_libraries(ember_anim_tests PRIVATE Ember::Anim)
