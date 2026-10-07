# The host runs a small game of its own: components, a prefab, and scripts given as text and compiled
# here. The asset suite drives a real manager over a scratch directory, as the asset tests do.
ember_add_test_suite(ember_script_tests script
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	dmath_tests.cpp
	host_tests.cpp
	source_tests.cpp
)

target_link_libraries(ember_script_tests PRIVATE Ember::Script Ember::Jobs Luau.VM)

if(EMBER_ENABLE_ASSETS)
	ember_add_test_suite(ember_script_asset_tests script
		"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
		script_asset_tests.cpp
	)

	target_link_libraries(ember_script_asset_tests PRIVATE Ember::ScriptAssets Ember::Jobs Ember::Gpu)
endif()
