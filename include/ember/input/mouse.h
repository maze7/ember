#pragma once

#include <bitset>
#include <ember/core/common.h>
#include <ember/platform/window.h>
#include <glm/glm.hpp>

namespace ember
{
	enum class MouseButton : i32
	{
		None,
		Left,
		Middle,
		Right,
		X1,
		X2,
		Count,
	};

	class Input;
	class Mouse final
	{
	public:
		static constexpr u16 BUTTON_COUNT = static_cast<u16>(MouseButton::Count);

		/// Mouse position, relative to the window, in Pixel coordinates.
		[[nodiscard]] glm::vec2 position() const noexcept { return m_position; }

		/// Motion accumulated this frame.
		[[nodiscard]] glm::vec2 delta() const noexcept { return m_delta; }

		/// Wheel movement accumulated this frame.
		[[nodiscard]] glm::vec2 wheel() const noexcept { return m_wheel; }

		/// Handle of the currently focused window.
		[[nodiscard]] WindowHandle window() const noexcept { return m_window; }

		/// Shorthand to see the `position()` x component
		[[nodiscard]] float x() const noexcept { return m_position.x; }

		/// Shorthand to see the `position()` y component
		[[nodiscard]] float y() const noexcept { return m_position.y; }

		/// Whether the given MouseButton is currently held down
		[[nodiscard]] bool down(MouseButton btn) const noexcept { return m_down[index(btn)]; }

		/// Whether the given MouseButton was pressed this frame
		[[nodiscard]] bool pressed(MouseButton btn) const noexcept { return m_pressed[index(btn)]; }

		/// Whether the given MouseButton was released this frame
		[[nodiscard]] bool released(MouseButton btn) const noexcept { return m_released[index(btn)]; }

		/// Returns the timestamp of when the MouseButton was last down
		[[nodiscard]] u64 button_timestamp(MouseButton btn) const noexcept { return m_timestamps[index(btn)]; }

		/// Returns the timestamp of the last mouse motion
		[[nodiscard]] u64 motion_timestamp() const noexcept { return m_motion_timestamp; }

		/// Returns the timestamp of the last input
		[[nodiscard]] u64 input_timestamp() const noexcept { return m_input_timestamp; }

	private:
		friend class Input;
		friend class InputState;

		using ButtonBits = std::bitset<BUTTON_COUNT>;

		[[nodiscard]] static constexpr u32 index(MouseButton btn) noexcept
		{
			const auto i = static_cast<u32>(btn);
			EMBER_ASSERT(i < BUTTON_COUNT);
			return i;
		}

		void next_frame(u64 /* timestamp */) noexcept
		{
			m_pressed.reset();
			m_released.reset();
			m_delta = {};
			m_wheel = {};
		}

		void clear() noexcept
		{
			m_down.reset();
			m_pressed.reset();
			m_released.reset();
			m_timestamps.fill(0);

			m_position = {};
			m_delta = {};
			m_wheel = {};
			m_window = {};

			m_motion_timestamp = 0;
			m_input_timestamp = 0;
		}

		void on_button(MouseButton btn, bool down, WindowHandle window, u64 timestamp) noexcept
		{
			u32 i = index(btn);
			bool was_down = m_down[i];

			if (down == was_down)
				return;

			m_down[i] = down;

			if (down)
				m_pressed[i] = true;
			else
				m_released[i] = true;

			m_timestamps[i] = timestamp;
			m_input_timestamp = timestamp;
			m_window = window;
		}

		void on_move(glm::vec2 position, glm::vec2 delta, WindowHandle window, u64 timestamp) noexcept
		{
			// Do not combine deltas from different window coordinate spaces.
			if (m_window != window)
				m_delta = {};

			m_position = position;
			m_delta += delta;
			m_window = window;
			m_motion_timestamp = timestamp;
			m_input_timestamp = timestamp;
		}

		void on_wheel(glm::vec2 wheel, WindowHandle window, u64 timestamp) noexcept
		{
			m_wheel += wheel;
			m_window = window;
			m_input_timestamp = timestamp;
		}

		void on_focus_lost(WindowHandle window, u64 timestamp) noexcept
		{
			if (!m_window.is_null() && m_window != window)
				return;

			m_released |= m_down;
			m_down.reset();
			m_delta = {};
			m_window = {};
			m_input_timestamp = timestamp;
		}

		/// whether a button was pressed this frame
		ButtonBits m_down{};

		/// whether a button was held this frame
		ButtonBits m_pressed{};

		/// whether a button was released this frame
		ButtonBits m_released{};

		/// timestamp of when a button was last pressed
		std::array<u64, BUTTON_COUNT> m_timestamps{};

		/// mouse position (window coordinates);
		glm::vec2 m_position = {0, 0};

		/// mouse wheel value for current frame
		glm::vec2 m_wheel = {0, 0};

		/// mouse delta value compared to previous position
		glm::vec2 m_delta = {0, 0};

		WindowHandle m_window{};

		u64 m_motion_timestamp = 0;
		u64 m_input_timestamp = 0;
	};
}
