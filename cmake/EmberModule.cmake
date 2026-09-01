include(CMakeParseArguments)

add_library(ember_build_options INTERFACE)

target_compile_options(ember_build_options INTERFACE
				$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
				$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->
)

function(ember_add_module name)
	cmake_parse_arguments(
								MODULE
								""
								""
								"SOURCES;PUBLIC_DEPS;PRIVATE_DEPS"
								${ARGN}
				)

	string(TOLOWER "${name}" name_lower)
	set(target "ember_${name_lower}")

	add_library(${target} STATIC ${MODULE_SOURCES})
	add_library(Ember::${name} ALIAS ${target})

	target_compile_features(${target} PUBLIC cxx_std_20)

	target_include_directories(${target}
		PUBLIC
			"$<BUILD_INTERFACE:${EMBER_ROOT}/include>"
			"$<INSTALL_INTERFACE:include>"
		PRIVATE
			"$<BUILD_INTERFACE:${EMBER_ROOT}/src>"
	)

	target_link_libraries(${target} PRIVATE ember_build_options)

	if(MODULE_PUBLIC_DEPS)
		target_link_libraries(${target} PUBLIC ${MODULE_PUBLIC_DEPS})
	endif()

	if(MODULE_PRIVATE_DEPS)
		target_link_libraries(${target} PRIVATE ${MODULE_PRIVATE_DEPS})
	endif()
endfunction()

# A game executable: the platform entry point and subsystem flags in one place.
# Console target properties land here once, later.
function(ember_add_game target)
	cmake_parse_arguments(GAME "" "" "SOURCES" ${ARGN})

	add_executable(${target} ${GAME_SOURCES})
	target_link_libraries(${target} PRIVATE Ember::Ember Ember::Main)

	if(WIN32)
		set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE ON)
	endif()
endfunction()
