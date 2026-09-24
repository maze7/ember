# Ember::Io is the file thread: reads queue from any thread and complete with a job signal.
ember_add_module(Io
	SOURCES
		file.cpp
	PUBLIC_DEPS
		Ember::Core
		Ember::Jobs
)
