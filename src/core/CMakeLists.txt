target_sources(ember_core PRIVATE
	common.cpp
	filesystem.cpp
	json.cpp
)

if(WIN32)
	target_sources(ember_core PRIVATE
		filesystem_win32.cpp
	)

	target_link_libraries(ember_core PRIVATE
		shell32
		ole32
	)
elseif(APPLE OR UNIX)
	target_sources(ember_core PRIVATE
		filesystem_posix.cpp
	)
else()
	message(FATAL_ERROR
		"Ember filesystem has no backend for this platform")
endif()
