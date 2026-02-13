#pragma once

#include "core/common.h"

namespace Ember
{
	enum class BlendFactor : u32
	{
		Zero,
		One,
		SrcColor,
		OneMinusSrcColor,
		DstColor,
		OneMinusDstColor,
		SrcAlpha,
		OneMinusSrcAlpha,
		DstAlpha,
		OneMinusDstAlpha,
		ConstantColor,
		OneMinusConstantColor,
		SrcAlphaSaturate
	};
}
