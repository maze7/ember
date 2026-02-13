#pragma once

#include "core/common.h"
#include "core/handle.h"
#include "graphics/enums/buffer_usage.h"
#include "graphics/enums/index_format.h"
#include "graphics/vertex.h"
#include <span>

namespace Ember
{
	struct Buffer {};

	struct BufferDef
	{
		BufferUsage usage;
		u32 size;
		std::span<const std::byte> data;
	};

	struct VertexBuffer
	{
		Handle<Buffer> handle = Handle<Buffer>::null;
		VertexFormat format;
	};

	struct IndexBuffer
	{
		Handle<Buffer> handle = Handle<Buffer>::null;
		IndexFormat format;
	};
}
