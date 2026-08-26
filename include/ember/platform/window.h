#pragma once

#include <ember/core/bitmask.h>
#include <ember/core/common.h>
#include <ember/core/handle.h>
#include <glm/glm.hpp>

namespace ember
{
	struct Window;
	using WindowHandle = Handle<Window>;

	enum class WindowFlags : u16
	{
		None			 = 0,
		Resizable		 = 1 << 0,
		Fullscreen		 = 1 << 1,
		Hidden			 = 1 << 3,
		Borderless		 = 1 << 4,
		HighPixelDensity = 1 << 5,
	};

	EMBER_ENUM_BITWISE_OPS(WindowFlags, u16);

	enum class WindowState : u16
	{
		None	   = 0,
		Visible	   = 1 << 0,
		Focused	   = 1 << 1,
		Minimized  = 1 << 2,
		Fullscreen = 1 << 3,
		Occluded   = 1 << 4,
	};

	EMBER_ENUM_BITWISE_OPS(WindowState, u16);

	enum class WindowError : u8
	{
		NotInitialized,
		InvalidDescription,
		InvalidHandle,
		Unsupported,
		BackendFailure
	};

	enum class WindowBackend : u8
	{
		None,
		Sdl3
	};

	enum class CursorMode : u8
	{
		Normal,
		Hidden,
		Relative,
	};

	struct WindowDef
	{
		const char* title = "Ember";
		glm::ivec2 size{1280, 720};

		WindowFlags flags = WindowFlags::Resizable | WindowFlags::HighPixelDensity;
	};

	struct NativeWindow
	{
		WindowBackend backend = WindowBackend::None;
		void* value = nullptr;

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return value != nullptr;
		}

		/// Returns true if the NativeWindow is of type `query`
		[[nodiscard]] bool is_type(WindowBackend query) const noexcept
		{
			return backend == query;
		}
	};
}
