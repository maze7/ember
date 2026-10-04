#pragma once

#include <ember/audio/engine.h>
#include <ember/core/filesystem.h>

#include <gtest/gtest.h>

#include <string>

// What the audio tests share: a game's banks, found where the build was told they are. The events
// they name are that game's: a footstep with a Surface parameter, a dodge, a sword's slash, and a
// bed of ambience that loops.
namespace ember::audio::test
{
	inline const std::string BANKS = EMBER_AUDIO_TEST_BANKS;

	[[nodiscard]] inline std::string bank(const char* name) { return BANKS + "/" + name; }

	[[nodiscard]] inline bool have_banks()
	{
		const auto found = fs::exists(bank("Master.bank"));
		return !BANKS.empty() && found && found.value();
	}

	/** An engine that plays to nowhere in step with the test, the banks loaded. */
	[[nodiscard]] inline bool start(Engine& engine, EngineDef def = {})
	{
		def.output = Output::Stepped;
		return engine.init(def) && engine.load_bank(bank("Master.strings.bank")) &&
			   engine.load_bank(bank("Master.bank"));
	}
}

#define EMBER_NEEDS_BANKS()                                                                                            \
	if (!ember::audio::test::have_banks())                                                                             \
	GTEST_SKIP() << "no banks: set EMBER_AUDIO_TEST_BANKS to a folder with a game's Master banks"
