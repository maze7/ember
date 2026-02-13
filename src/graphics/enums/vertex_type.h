#pragma once

#include "core/common.h"

namespace Ember
{
	enum class VertexType
	{
		None,
		Float,
		Float2,
		Float3,
		Float4,
		Byte4,
		UByte4,
		Short2,
		UShort2,
		Short4,
		UShort4
	};

	/**
	 * @brief Utility function to get the size (in bytes) of a VertexType
	 * @returns Number of bytes for given VertexType
	 */
	inline int vertex_type_size(VertexType type) {
		switch (type) {
			case VertexType::Float:
				return 4;
			case VertexType::Float2:
				return 8;
			case VertexType::Float3:
				return 12;
			case VertexType::Float4:
				return 16;
			case VertexType::Byte4:
				return 4;
			case VertexType::UByte4:
				return 4;
			case VertexType::Short2:
				return 4;
			case VertexType::UShort2:
				return 4;
			case VertexType::Short4:
				return 8;
			case VertexType::UShort4:
				return 8;
			default:
				throw Exception("Unknown vertex type!");
		}
	}
}
