#pragma once

#include <ember/core/common.h>
#include <ember/core/bitmask.h>
#include <glm/glm.hpp>

namespace ember
{
	enum class WindowFlags : u16
	{
		None			 = 0,
		Resizable		 = 1 << 0,
		Fullscreen		 = 1 << 1,
		Hidden			 = 1 << 3,
		Borderless		 = 1 << 3,
		HighPixelDensity = 1 << 4,
	};

	EMBER_ENUM_BITWISE_OPS(WindowFlags, u16);

	struct WindowDef
	{
		std::string_view title = "Ember";
		glm::vec2 size{1280, 720};
		WindowFlags flags = WindowFlags::Resizable;
	};

	enum class WindowError : u8
	{
		NotInitialized,
		InvalidDescription,
		InvalidHandle,
		Unsupported,
		BackendFailure
	};

	enum class CursorMode : u8
	{
		Normal,
		Hidden,
		Relative,
	};
}
