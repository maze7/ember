#pragma once

#include <ember/core/common.h>
#include <ember/core/handle.h>
#include <glm/glm.hpp>

#include <span>

namespace ember
{
	struct Cursor;
	using CursorHandle = Handle<Cursor>;

	enum class SystemCursor : u8
	{
		Default,
		Text,
		Wait,
		Crosshair,
		Progress,
		ResizeNWSE,
		ResizeNESW,
		ResizeHorizontal,
		ResizeVertical,
		Move,
		NotAllowed,
		Pointer,
		ResizeNW,
		ResizeN,
		ResizeNE,
		ResizeE,
		ResizeSE,
		ResizeS,
		ResizeSW,
		ResizeW,
	};

	struct CursorImageView
	{
		std::span<const std::byte> rgba8;
		glm::ivec2 size{};
		glm::ivec2 hotspot{};
		u32 pitch = 0;
	};
}
