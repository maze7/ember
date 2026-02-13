#pragma once

#include "core/common.h"

namespace Ember
{
	enum class DepthCompare : u32
	{
		Always,
		Never,
		Less,
		Equal,
		LessOrEqual,
		Greater,
		NotEqual,
		GreaterOrEqual,
	};
}
