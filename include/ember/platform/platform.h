#pragma once

#include <ember/memory/common.h>
#include <ember/platform/window.h>
#include <ember/core/result.h>

namespace ember
{
	class Platform final
	{
	public:
		Platform() = default;

		Result<WindowHandle, WindowError> create_window(const WindowDef& def) noexcept;
		Result<void, WindowError> destroy_window(WindowHandle handle) noexcept;

		void set_window_title(WindowHandle handle, std::string_view title) noexcept;
		void set_window_size(WindowHandle handle, u32 width, u32 height) noexcept;

	private:
		struct Impl;
		Impl* m_impl = nullptr;
	};
}
