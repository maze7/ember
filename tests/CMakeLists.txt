include(GoogleTest)

add_executable(ember_memory_tests
	main.cpp
	memory/memory_tests.cpp
	memory/virtual_memory_tests.cpp
	memory/pmr/arena_resource_tests.cpp
	memory/pmr/heap_resource_tests.cpp
)

target_link_libraries(ember_memory_tests
	PRIVATE
		Ember::Core
		GTest::gtest
		ember_build_options # test code compiles under the engine's warning set
)

# Registers every TEST() as an individual CTest entry, so ctest can run, filter
# and parallelize them (ctest -L memory, ctest -j).
gtest_discover_tests(ember_memory_tests
	PROPERTIES LABELS "memory"
	DISCOVERY_TIMEOUT 30
)
