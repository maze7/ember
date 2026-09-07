# Ember::Jobs, the fiber job system
ember_add_module(Jobs
	PUBLIC_DEPS
	Ember::Core
)

if (NOT WIN32)
	enable_language(ASM)

	target_sources(ember_jobs
		PRIVATE
			fiber_posix.cpp
			fiber_x86_64_sysv.S
	)
endif()
