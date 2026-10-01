# Ember::Net, the netcode: wire format (serialize), connections, commands and the clock, transports
# (Valve's networking sockets, loopback). The sockets library stays private: no header of it
# reaches the engine's public include tree.
ember_add_module(Net
	SOURCES
		# command_stream.cpp
		# common.cpp
		connection.cpp
		# loopback.cpp
		valve_transport.cpp
		# time_dilation.cpp
	PUBLIC_DEPS
		Ember::Core
		serialize::serialize
	PRIVATE_DEPS
		Valve::NetworkingSockets
)

target_compile_definitions(ember_net PUBLIC EMBER_USE_STEAM=$<BOOL:${EMBER_USE_STEAM}>)

# The sockets library's objects are built without RTTI (GameNetworkingSockets) or outside this build
# (Steam), so UBSan's vptr check cannot recognise them and would stop every call into the library
# under the asan preset. Only sockets.cpp calls in.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	set_source_files_properties(sockets.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=vptr")
endif()
