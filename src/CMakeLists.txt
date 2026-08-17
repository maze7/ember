# Ember::Core module
ember_add_module(Core
	PUBLIC_DEPS
		fmt::fmt
		tl::expected
		glm::glm
		unordered_dense::unordered_dense
		$<$<BOOL:${EMBER_USE_RPMALLOC}>:rpmalloc::rpmalloc>
		$<$<BOOL:${EMBER_USE_TRACY}>:Tracy::TracyClient>
)

# Ember Core sources files
add_subdirectory(core)
add_subdirectory(memory)
add_subdirectory(sync)

if(WIN32)
	target_compile_definitions(ember_core PUBLIC EMBER_PLATFORM_WINDOWS)
elseif(APPLE)
	target_compile_definitions(ember_core PUBLIC EMBER_PLATFORM_MACOS)
elseif(UNIX)
	target_compile_definitions(ember_core PUBLIC EMBER_PLATFORM_LINUX)
endif()

# Ember Core compile definitions
target_compile_definitions(ember_core PUBLIC
	EMBER_USE_TRACY=$<BOOL:${EMBER_USE_TRACY}>
	EMBER_USE_RPMALLOC=$<BOOL:${EMBER_USE_RPMALLOC}>
	# Memory tracking
	$<$<CONFIG:Debug>:EMBER_MEMORY_TRACKING=2>
	$<$<CONFIG:Profile>:EMBER_MEMORY_TRACKING=1>
)

if(EMBER_ENABLE_PLATFORM)
	ember_add_module(Platform
		PUBLIC_DEPS
			Ember::Core
		PRIVATE_DEPS
			SDL3::SDL3
	)

	add_subdirectory(input)
	add_subdirectory(platform)
endif()

add_library(ember INTERFACE)
add_library(Ember::Ember ALIAS ember)

if (EMBER_ENABLE_GRAPHICS)
	add_subdirectory(gpu)
endif()

if (EMBER_ENABLE_AUDIO)
	add_subdirectory(audio)
endif()

target_link_libraries(ember INTERFACE
	Ember::Core
	$<$<BOOL:${EMBER_ENABLE_PLATFORM}>:Ember::Platform>
	$<$<BOOL:${EMBER_ENABLE_GRAPHICS}>:Ember::Gpu>
	$<$<BOOL:${EMBER_ENABLE_AUDIO}>:Ember::Audio>
)
