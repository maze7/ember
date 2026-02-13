#pragma once

#include "core/common.h"

namespace Ember
{
	enum class ClearMask : i32
	{
		None    = 0,
		Color   = 1,
		Depth   = 2,
		Stencil = 4,
		All     = Color | Depth | Stencil
	};
}
