target_sources(ember_core
	PRIVATE
		memory.cpp
		memory_tracker.cpp
		virtual_memory.cpp
		pmr/arena_resource.cpp
		pmr/heap_resource.cpp
		# Virtual memory backend differs per platform, swapped at compile time.
		$<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/virtual_memory_win32.cpp>
		$<$<PLATFORM_ID:Darwin,Linux>:${CMAKE_CURRENT_SOURCE_DIR}/virtual_memory_posix.cpp>
)
