# Asset identity is header only and needs nothing but Core; the module's own tests join this
# suite as it grows (file IO, then the manager).
ember_add_test_suite(ember_asset_tests assets
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	asset_id_tests.cpp
)
