# Ember's command line tools. The cook compiles Slang, so it comes with graphics, like the compiler.
if(EMBER_ENABLE_GRAPHICS AND EMBER_COOK)
	add_subdirectory(cook)
endif()
