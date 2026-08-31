#pragma once

#include <ember/containers/span.h>

namespace ember
{
	/// What the platform entry point hands the game: UTF-8 args on every platform,
	/// argv[0] first.
	struct Args
	{
		Span<const char* const> args;
	};
}

/// Defined by the game. Ember::Main owns the real entry point per platform,
/// normalizes what the OS provides, and calls this.
int ember_main(const ember::Args& launch);
