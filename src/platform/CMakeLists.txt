target_sources(ember_platform
	PRIVATE
		sdl3/platform_sdl3.cpp
		$<$<BOOL:${EMBER_USE_VULKAN}>:${CMAKE_CURRENT_SOURCE_DIR}/vulkan/wsi_sdl3.cpp>
)

target_link_libraries(ember_platform
	PRIVATE
		$<$<BOOL:${EMBER_USE_VULKAN}>:volk::volk_headers>
)

target_compile_definitions(ember_platform
	PUBLIC
		# Platform needs to be aware of the render backend so we can
		# provide required flags to the SDL_Window on creation.
		EMBER_USE_VULKAN=$<BOOL:${EMBER_USE_VULKAN}>
)
