# The manager drives real loader jobs, the file thread and, for the watched suite, a real OS
# watch over a scratch directory. The device it is given is headless and may not even boot; the
# test asset type never touches it.
ember_add_test_suite(ember_asset_tests assets
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	asset_id_tests.cpp
	asset_manager_tests.cpp
)

target_link_libraries(ember_asset_tests PRIVATE Ember::Assets Ember::Jobs Ember::Gpu)
