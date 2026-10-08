# Ember::Script: Luau for gameplay. A world owns one Host, a VM that runs the scripts a game loads
# into it; the Binding exposes components by their fields and the game's own functions; scripts are
# Sources, compiled once by whoever read the text, with the lints the parse found; a game's schedule
# runs the host's systems through exclusive script points. Headless: a dedicated server links it.
# Luau's VM headers reach the game through <ember/script/lua.h>, for binding packs that write C
# functions; nothing else of Luau does.
ember_add_module(Script
	SOURCES
		binding.cpp
		declare.cpp
		definitions.cpp
		dmath.cpp
		host.cpp
		modifier.cpp
		panel.cpp
		physics_binding.cpp
		present.cpp
		query.cpp
		schema.cpp
		source.cpp
		stategraph.cpp
		story.cpp
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

# Ember::ScriptUi: a client's debug UI for its scripts, in Dear ImGui: the `ui` library `panel` frames draw
# with, the windows they open in, and an inspector's view of a host's stategraphs, stories and modifiers.
# A client's tools link it; a dedicated server never does.
if(EMBER_ENABLE_IMGUI AND EMBER_ENABLE_GRAPHICS)
	ember_add_module(ScriptUi
		SOURCES
			ui.cpp
		PUBLIC_DEPS
			Ember::Script
		PRIVATE_DEPS
			imgui::imgui
	)
endif()
