include(GoogleTest)

function(ember_add_test_suite target label)
	add_executable(${target} ${ARGN})

	target_link_libraries(${target}
		PRIVATE
			Ember::Core
			GTest::gtest
			ember_build_options
	)

	gtest_discover_tests(${target}
		PROPERTIES LABELS "${label}"
		DISCOVERY_TIMEOUT 30
	)
endfunction()

add_subdirectory(containers)
add_subdirectory(memory)
