#pragma once

#include "core/common.h"

namespace Ember
{
	enum class TextureFormat
	{
		R8G8B8A8,			// Red = 8, Green = 8, Blue = 8, Alpha = 8
		R8,					// Red = 8
		Depth24Stencil8,	// Depth = 24, Stencil = 8
		Color,				// Shorthand for R8G8B8A8
	};

	inline int texture_format_size(TextureFormat format) {
    	switch (format) {
    		case TextureFormat::R8G8B8A8:
    			return 4;
    		case TextureFormat::R8:
    			return 1;
    		case TextureFormat::Depth24Stencil8:
    			return 4;
    		case TextureFormat::Color:
    			return 4;
    		default:
    			throw Exception("Unknown texture format");
    	}
	}
}
