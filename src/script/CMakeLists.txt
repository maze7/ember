# Ember::Script: Luau for gameplay. A world owns one Host, a VM that runs the scripts a game loads
# into it; the Binding exposes components by their fields and the game's own functions; scripts are
# Sources, compiled once by whoever read the text, with the lints the parse found; a game's schedule
# runs the host's systems through exclusive script points. Headless: a dedicated server links it.
# Luau's VM headers reach the game through <ember/script/lua.h>, for binding packs that write C
# functions; nothing else of Luau does.
ember_add_module(Script
	SOURCES
		binding.cpp
		definitions.cpp
		dmath.cpp
		host.cpp
		physics_binding.cpp
		query.cpp
		source.cpp
	PUBLIC_DEPS
		Ember::Core
		Ember::Ecs
		Ember::Physics
		Luau.VM
	PRIVATE_DEPS
		Luau.Compiler
		Luau.Ast
		Ember::Jobs
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	# The deterministic math and the field conversions must add and multiply as the VM does.
	target_compile_options(ember_script PRIVATE -ffp-contract=off)
endif()

# Ember::ScriptAssets: scripts as assets. The ScriptAsset type compiles on a loader job, and the
# ScriptLibrary turns the manager's reloads into the batches of sources a host takes. A client,
# the runner and the editor link it; a dedicated server has no asset manager and compiles from disk.
if(EMBER_ENABLE_ASSETS)
	ember_add_module(ScriptAssets
		SOURCES
			script_asset.cpp
		PUBLIC_DEPS
			Ember::Script
			Ember::Assets
		PRIVATE_DEPS
			Ember::Jobs
	)
endif()
