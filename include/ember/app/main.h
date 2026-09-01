#pragma once

#include <ember/containers/span.h>

namespace ember
{
	/// What the platform entry point hands the game: UTF-8 args on every platform.
	struct Args
	{
		Span<const char* const> args;
	};

}

/// Defined by the game, usually by EMBER_GAME. Owns everything between entry and exit.
int ember_main(const ember::Args& args);
