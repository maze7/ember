# The engine plays to nowhere, a mix block an update, so a test hears exactly what it asked for. The
# sounds are a game's: EMBER_AUDIO_TEST_BANKS is a folder holding the Master and Master.strings banks
# whose events these tests name, and the tests that need a bank skip without it.
set(EMBER_AUDIO_TEST_BANKS "" CACHE PATH "A folder holding a game's Master.bank and Master.strings.bank, for the audio tests")

ember_add_test_suite(ember_audio_tests audio
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	engine_tests.cpp
	emitter_tests.cpp
	cue_tests.cpp
)

target_link_libraries(ember_audio_tests PRIVATE Ember::Audio Ember::Anim Ember::Ecs Ember::Jobs)
target_compile_definitions(ember_audio_tests PRIVATE EMBER_AUDIO_TEST_BANKS="${EMBER_AUDIO_TEST_BANKS}")
