# Ember::Audio: what a designer makes in FMOD Studio, played. FMOD is private to it, and only a build
# with EMBER_ENABLE_AUDIO has it: without, the same engine does nothing, so games are written once.
# The components and names are plain headers, which a server includes without linking any of this.
if(EMBER_ENABLE_AUDIO)
	set(EMBER_AUDIO_ENGINE engine_fmod.cpp)
else()
	set(EMBER_AUDIO_ENGINE engine_null.cpp)
endif()

ember_add_module(Audio
	SOURCES
		${EMBER_AUDIO_ENGINE}
		systems.cpp
	PUBLIC_DEPS
		Ember::Core
		Ember::Ecs
)

if(EMBER_ENABLE_AUDIO)
	target_link_libraries(ember_audio PRIVATE FMOD::Studio)
endif()
