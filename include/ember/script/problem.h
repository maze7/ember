#pragma once

#include <ember/core/common.h>
#include <ember/memory/memory.h>

namespace ember::script
{
	enum class Severity : u8
	{
		Error,	 // the module is refused, or the function that raised it is off until its module reloads
		Warning, // worth a look; nothing stops
		Count
	};

	/**
	 * Something a script did wrong, and where: what the log and the script panel show, and what the
	 * tests assert on. One record a place: the same problem again raises `count`, so a handler that
	 * fails every tick is one line, not a flood.
	 */
	struct Problem
	{
		String path{&memory::heap(MemoryTag::Scripting)}; // the script, as assets name it
		u32 line		  = 0;							  // 1 based; 0 when the whole file is meant
		u32 column		  = 0;							  // 1 based; 0 when unknown
		Severity severity = Severity::Error;
		String message{&memory::heap(MemoryTag::Scripting)};
		u32 count = 1;

		[[nodiscard]] bool same_place(const Problem& other) const noexcept
		{
			return path == other.path && line == other.line && message == other.message;
		}
	};
}

namespace ember
{
	EMBER_ENUM_NAMES(script::Severity, "Error", "Warning");
}
