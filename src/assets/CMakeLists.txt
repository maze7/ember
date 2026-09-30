# Ember::Assets: the registry, its loader jobs, the engine's own asset types and the file watch
# behind hot reload.
if (NOT EMBER_ENABLE_GRAPHICS)
	message(FATAL_ERROR "EMBER_ENABLE_ASSETS requires EMBER_ENABLE_GRAPHICS")
endif()

ember_add_module(Assets
	SOURCES
		asset_manager.cpp
		texture_asset.cpp
		file_watcher.cpp
		material_asset.cpp
	PUBLIC_DEPS
		Ember::Core
		Ember::Jobs
		Ember::Gpu
		Ember::Render
		Ember::Material
	PRIVATE_DEPS
		Stb::Stb
		efsw::efsw
		$<$<BOOL:${EMBER_SHADER_COMPILER}>:Ember::ShaderCompiler>
)

# Material types build from source, and hot reload, only in builds with the compiler; ship builds
# read the pairs ember_cook wrote, and never link libslang.
target_compile_definitions(ember_assets PRIVATE EMBER_SHADER_COMPILER=$<BOOL:${EMBER_SHADER_COMPILER}>)
