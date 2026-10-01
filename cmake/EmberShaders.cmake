# Shader build support. Every Slang file the engine or a game cooks goes through ember_cook, the
# command line face of shader::Compiler, so what the build cooks and what hot reload compiles come
# from one set of options, kept in one place: the compiler. ember_embed_shaders links cooked files
# into an engine module; games cook the files they load from disk with the functions below.
#
# Paths resolve from this file's location into INTERNAL cache entries, so the functions work from
# any directory scope, game trees included.

get_filename_component(_ember_shaders_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(EMBER_SHADER_SOURCE_DIR "${_ember_shaders_root}/shaders" CACHE INTERNAL "")
set(EMBER_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EmbedBlob.cmake" CACHE INTERNAL "")

# One ember_cook run: <output>, plus the OUTPUTS the tool writes beside it. The tool writes a
# depfile naming every file the compile read, so an edit to an engine module or a game library
# recooks exactly the files that import it, and no dependency list is kept by hand. DEPENDS on the
# tool recooks everything when the compiler changes; an output whose bytes did not change keeps its
# timestamp, and the build stops there.
function(_ember_cook kind output source)
	cmake_parse_arguments(ARG "" "" "OUTPUTS;INCLUDE_DIRS" ${ARGN})

	# Without the cook the files are sources: whatever a build with it on left behind.
	if(NOT EMBER_COOK)
		foreach(file IN ITEMS "${output}" ${ARG_OUTPUTS})
			if(NOT EXISTS "${file}")
				message(FATAL_ERROR "${file} is not cooked and EMBER_COOK is off: build with the cook on first")
			endif()
		endforeach()
		return()
	endif()

	# Relative paths name files in the calling directory, as they do everywhere else in CMake; the
	# tool itself runs in the binary directory.
	cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
	cmake_path(GET source FILENAME source_name)

	set(includes)
	foreach(dir IN LISTS ARG_INCLUDE_DIRS)
		cmake_path(ABSOLUTE_PATH dir BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
		list(APPEND includes -I "${dir}")
	endforeach()

	add_custom_command(
		OUTPUT "${output}" ${ARG_OUTPUTS}
		COMMAND ember_cook ${kind} "${source}" -o "${output}" --depfile "${output}.d" ${includes}
		DEPFILE "${output}.d"
		DEPENDS "${source}" ember_cook
		COMMENT "cook ${source_name}"
		VERBATIM
	)
endfunction()

# ember_cook_shader(<output.spv> <source.slang> [INCLUDE_DIRS <dir>...])
#
# Cooks one plain program, a file with its own entry points, to SPIR-V. INCLUDE_DIRS hold the
# game's shader libraries, searched for imports after the file's own directory and the engine's.
function(ember_cook_shader output source)
	_ember_cook(program "${output}" "${source}" ${ARGN})
endfunction()

# ember_cook_material(<output.spv> <source.slang> [INCLUDE_DIRS <dir>...])
#
# Cooks one material type to its pair: the SPIR-V, and the .type file ember_cook writes beside it.
# INCLUDE_DIRS as for ember_cook_shader.
function(ember_cook_material output source)
	cmake_path(REPLACE_EXTENSION output LAST_ONLY ".type" OUTPUT_VARIABLE type)
	_ember_cook(material "${output}" "${source}" OUTPUTS "${type}" ${ARGN})
endfunction()

# Links one cooked file into the target as <namespace>::<symbol>(), through a generated TU that
# includes the declaring header, so a declaration that drifts fails to compile rather than to link.
function(_ember_embed target file symbol namespace header)
	set(generated "${CMAKE_CURRENT_BINARY_DIR}/embedded/${symbol}.cpp")

	add_custom_command(
		OUTPUT "${generated}"
		COMMAND "${CMAKE_COMMAND}"
			"-DINPUT=${file}"
			"-DOUTPUT=${generated}"
			"-DSYMBOL=${symbol}"
			"-DNAMESPACE=${namespace}"
			"-DHEADER=${header}"
			-P "${EMBER_EMBED_SCRIPT}"
		DEPENDS "${file}" "${EMBER_EMBED_SCRIPT}"
		COMMENT "embed ${symbol}"
		VERBATIM
	)

	target_sources(${target} PRIVATE "${generated}")
endfunction()

# ember_embed_shaders(<target>
#     NAMESPACE <c++ namespace for the accessors>
#     HEADER    <declaring header, as included>
#     [SHADERS   <foo.slang ...>]
#     [MATERIALS <dir/bar.slang ...>])
#
# Cooks files from the engine's shader directory and links them into the target: foo_shader() for
# a plain program's SPIR-V, and for a material type bar_material_spirv() and bar_material_type(),
# the pair material::read_cooked joins.
function(ember_embed_shaders target)
	cmake_parse_arguments(ARG "" "NAMESPACE;HEADER" "SHADERS;MATERIALS" ${ARGN})

	if(NOT ARG_NAMESPACE OR NOT ARG_HEADER OR NOT (ARG_SHADERS OR ARG_MATERIALS))
		message(FATAL_ERROR "ember_embed_shaders(${target}): NAMESPACE, HEADER and SHADERS or MATERIALS are required")
	endif()

	# The build tree, unless the game named a directory in its source tree (EMBER_COOKED_DIR, set
	# before adding the engine), where a build that cannot cook finds them.
	if(EMBER_COOKED_DIR)
		set(cooked "${EMBER_COOKED_DIR}")
	else()
		set(cooked "${CMAKE_CURRENT_BINARY_DIR}/shaders")
	endif()

	foreach(shader IN LISTS ARG_SHADERS)
		cmake_path(REMOVE_EXTENSION shader LAST_ONLY OUTPUT_VARIABLE stem)
		cmake_path(GET shader STEM LAST_ONLY name)

		ember_cook_shader("${cooked}/${stem}.spv" "${EMBER_SHADER_SOURCE_DIR}/${shader}")
		_ember_embed(${target} "${cooked}/${stem}.spv" "${name}_shader" "${ARG_NAMESPACE}" "${ARG_HEADER}")
	endforeach()

	foreach(material IN LISTS ARG_MATERIALS)
		cmake_path(REMOVE_EXTENSION material LAST_ONLY OUTPUT_VARIABLE stem)
		cmake_path(GET material STEM LAST_ONLY name)

		ember_cook_material("${cooked}/${stem}.spv" "${EMBER_SHADER_SOURCE_DIR}/${material}")
		_ember_embed(${target} "${cooked}/${stem}.spv" "${name}_material_spirv" "${ARG_NAMESPACE}" "${ARG_HEADER}")
		_ember_embed(${target} "${cooked}/${stem}.type" "${name}_material_type" "${ARG_NAMESPACE}" "${ARG_HEADER}")
	endforeach()
endfunction()
