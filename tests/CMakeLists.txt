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

add_subdirectory(core)
add_subdirectory(containers)
add_subdirectory(memory)
add_subdirectory(math)
add_subdirectory(jobs)
add_subdirectory(ecs)
add_subdirectory(physics)
add_subdirectory(script)

if(EMBER_ENABLE_ASSETS)
	add_subdirectory(assets)
	add_subdirectory(anim)
endif()

# The audio tests play through FMOD, and sample animations as a game does.
if(EMBER_ENABLE_AUDIO AND EMBER_ENABLE_ASSETS)
	add_subdirectory(audio)
endif()

if(EMBER_ENABLE_APP)
	add_subdirectory(app)
endif()

if(EMBER_ENABLE_NET)
	add_subdirectory(net)
endif()

if(EMBER_ENABLE_GRAPHICS)
	add_subdirectory(gpu)
	add_subdirectory(render)
	add_subdirectory(material)
	add_subdirectory(shader)
endif()
