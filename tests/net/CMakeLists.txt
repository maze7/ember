ember_add_test_suite(ember_net_tests net
	"${CMAKE_CURRENT_LIST_DIR}/../main.cpp"
	command_stream_tests.cpp
	connection_tests.cpp
	loopback_tests.cpp
	serialize_tests.cpp
	session_clock_tests.cpp
	valve_transport_tests.cpp
	tick_ring_tests.cpp
	time_dilation_tests.cpp
)

target_link_libraries(ember_net_tests PRIVATE Ember::Net)

# The hostile peer in valve_transport_tests.cpp talks to the sockets library directly, and a Steam build's
# tests start Steam themselves.
target_link_libraries(ember_net_tests PRIVATE Valve::NetworkingSockets)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	set_source_files_properties(valve_transport_tests.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=vptr")
endif()

# A Steam build's tests load steam_api from beside them, as the game will; before the link, because
# test discovery runs the executable straight after it.
if(WIN32 AND EMBER_USE_STEAM)
	add_custom_command(TARGET ember_net_tests PRE_LINK
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:steam_api>" "$<TARGET_FILE_DIR:ember_net_tests>"
	)
endif()
