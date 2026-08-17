#pragma once

#include <ember/core/result.h>
#include <ember/input/input.h>
#include <ember/input/cursor.h>
#include <ember/platform/window.h>

namespace ember
{
	class Input;

	struct PumpResult
	{
		bool quit_requested = false;
	};

	class Platform final
	{
	public:
		Platform() noexcept;
		~Platform() noexcept;

		Platform(const Platform&) = delete;
		Platform& operator=(const Platform&) = delete;

		Platform(Platform&& other) noexcept;
		Platform& operator=(Platform&& other) noexcept;

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_impl != nullptr;
		}

		/**
		 * Drains SDL once, folds physical input into Input::NextState,
		 * and publishes Input::State before returning.
		 *
		 * Must be called exactly once per update from the Platform owner thread.
		 */
		[[nodiscard]] PumpResult pump_events(Input& input) noexcept;

		WindowHandle create_window(const WindowDef& def) noexcept;
		void destroy_window(WindowHandle handle) noexcept;

		void set_window_title(WindowHandle handle, const char* title) noexcept;
		void set_window_size(WindowHandle handle, u32 width, u32 height) noexcept;
		void set_cursor_mode(WindowHandle window, CursorMode mode) noexcept;

		CursorHandle create_system_cursor(SystemCursor cursor) noexcept;
		CursorHandle create_cursor(const CursorImageView& image) noexcept;
		void set_cursor(CursorHandle cursor) noexcept;
		void destroy_cursor(CursorHandle cursor) noexcept;
		void reset_cursor() noexcept;
		void set_cursor_visible(bool visible) noexcept;

		void start_text_input(WindowHandle window) noexcept;
		void stop_text_input(WindowHandle window) noexcept;

		void set_clipboard_text(const char* text) noexcept;
		std::string clipboard_text() const noexcept;

		void rumble(GamepadId gamepad, f32 low_intensity, f32 high_intensity, u32 duration_ms) noexcept;

	private:
		// PImpl is used to keep SDL out of public headers.
		struct Impl;

		void reset() noexcept;

		Impl* m_impl = nullptr;
	};
}
