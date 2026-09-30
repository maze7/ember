# What a material type is, without a compiler: the .type file, value parsing and hashing.
ember_add_test_suite(ember_material_tests material
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	type_tests.cpp
)

target_link_libraries(ember_material_tests PRIVATE Ember::Material)
