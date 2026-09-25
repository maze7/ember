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
	PUBLIC_DEPS
		Ember::Core
		Ember::Jobs
		Ember::Io
		Ember::Gpu
	PRIVATE_DEPS
		Stb::Stb
		efsw::efsw
)
