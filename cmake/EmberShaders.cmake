# Shader build support. ember_cook_shader is the one home for the slangc
# contract; ember's modules embed cooked SPIR-V into their targets through
# ember_embed_shaders, and game cooks call ember_cook_shader for shaders they
# load from disk instead.
#
# Paths resolve from this file's location into INTERNAL cache entries, so the
# functions work from any directory scope, game trees included.

find_program(EMBER_SLANGC slangc HINTS "$ENV{VULKAN_SDK}/bin")

get_filename_component(_ember_shaders_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(EMBER_SHADER_SOURCE_DIR "${_ember_shaders_root}/shaders" CACHE INTERNAL "")
set(EMBER_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EmbedBlob.cmake" CACHE INTERNAL "")

# ember_cook_shader(<output.spv> <source.slang> [DEPENDS <includes...>])
#
# DEPENDS defaults to the engine prelude every shader sees; a superset
# dependency only costs a spare recook.
function(ember_cook_shader output source)
	cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})

	if(NOT EMBER_SLANGC)
		message(FATAL_ERROR "ember_cook_shader: slangc not found; install the Vulkan SDK or set EMBER_SLANGC")
	endif()

	if(NOT ARG_DEPENDS)
		set(ARG_DEPENDS
			"${EMBER_SHADER_SOURCE_DIR}/ember.slang"
			"${EMBER_SHADER_SOURCE_DIR}/render.slang"
		)
	endif()

	cmake_path(GET source FILENAME source_name)

	add_custom_command(
		OUTPUT "${output}"
		COMMAND "${EMBER_SLANGC}" "${source}"
			-target spirv
			-fvk-use-entrypoint-name
			-matrix-layout-column-major
			# Typed aliases over one bindless binding are the contract
			# (EMBER_BUFFER_ALIAS); 39001 flags exactly that overlap.
			-Wno-39001
			-I "${EMBER_SHADER_SOURCE_DIR}"
			-o "${output}"
		DEPENDS "${source}" ${ARG_DEPENDS}
		COMMENT "slangc ${source_name}"
		VERBATIM
	)
endfunction()

# ember_embed_shaders(<target>
#     NAMESPACE <c++ namespace for the accessors>
#     HEADER    <declaring header, as included>
#     SHADERS   <foo.slang ...>
#     [DEPENDS  <extra include dependencies>])
#
# For each foo.slang: cook, generate foo_spv.cpp defining
# <NAMESPACE>::foo_shader(), and add it to the target. The generated TU
# includes HEADER, so declaration drift fails to compile, not to link.
function(ember_embed_shaders target)
	cmake_parse_arguments(ARG "" "NAMESPACE;HEADER" "SHADERS;DEPENDS" ${ARGN})

	if(NOT ARG_NAMESPACE OR NOT ARG_HEADER OR NOT ARG_SHADERS)
		message(FATAL_ERROR "ember_embed_shaders(${target}): NAMESPACE, HEADER and SHADERS are required")
	endif()

	set(spv_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
	set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/embedded")

	foreach(shader IN LISTS ARG_SHADERS)
		cmake_path(REMOVE_EXTENSION shader OUTPUT_VARIABLE stem)
		set(spv "${spv_dir}/${stem}.spv")
		set(generated "${gen_dir}/${stem}_spv.cpp")

		if(ARG_DEPENDS)
			ember_cook_shader("${spv}" "${EMBER_SHADER_SOURCE_DIR}/${shader}" DEPENDS ${ARG_DEPENDS})
		else()
			ember_cook_shader("${spv}" "${EMBER_SHADER_SOURCE_DIR}/${shader}")
		endif()

		add_custom_command(
			OUTPUT "${generated}"
			COMMAND "${CMAKE_COMMAND}"
				"-DINPUT=${spv}"
				"-DOUTPUT=${generated}"
				"-DSYMBOL=${stem}_shader"
				"-DNAMESPACE=${ARG_NAMESPACE}"
				"-DHEADER=${ARG_HEADER}"
				-P "${EMBER_EMBED_SCRIPT}"
			DEPENDS "${spv}" "${EMBER_EMBED_SCRIPT}"
			COMMENT "embed ${stem}.spv"
			VERBATIM
		)

		target_sources(${target} PRIVATE "${generated}")
	endforeach()
endfunction()
