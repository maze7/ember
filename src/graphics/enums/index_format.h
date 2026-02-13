#pragma once

#include "core/common.h"

namespace Ember
{
	enum class IndexFormat
	{
		Sixteen,
		ThirtyTwo
	};

	inline u8 index_format_size(IndexFormat format) {
		switch (format) {
			case IndexFormat::Sixteen: return 2;
			case IndexFormat::ThirtyTwo: return 4;
		}
	}
}
