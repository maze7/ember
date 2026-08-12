# Ember Core module
ember_add_module(Core
	PUBLIC_DEPS
		fmt::fmt
		$<$<BOOL:${EMBER_USE_TRACY}>:Tracy::TracyClient>
)

# Ember Core sources files
add_subdirectory(core)
add_subdirectory(memory)

# Ember Core compile definitions
target_compile_definitions(ember_core PUBLIC
	EMBER_USE_TRACY=$<BOOL:${EMBER_USE_TRACY}>
)

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
