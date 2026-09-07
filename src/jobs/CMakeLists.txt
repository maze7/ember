# Ember::Jobs, the fiber job system
ember_add_module(Jobs
	PUBLIC_DEPS
		Ember::Core
)

if(WIN32)
	target_sources(ember_jobs
		PRIVATE
			fiber_win32.cpp
	)
else()
	enable_language(ASM)

	target_sources(ember_jobs
		PRIVATE
			fiber_posix.cpp
			fiber_x86_64_sysv.S
			fiber_aarch64_aapcs.S
	)
endif()
