#pragma once

#include <cstring>
#include <ember/core/common.h>
#include <glm/glm.hpp>

namespace ember
{
	enum class MouseButton : i32
	{
		None = 0,
		Left = 1,
		Middle = 2,
		Right = 3
	};

	class Input;
	class Mouse
	{
	public:
		static constexpr u32 MAX_MOUSE_BUTTONS = 8;

		/// Mouse position, relative to the window, in Pixel coordinates.
		[[nodiscard]] glm::vec2 position() const { return m_position; }

		/// Motion accumulated this frame.
		[[nodiscard]] glm::vec2 delta() const { return m_delta; }

		/// Wheel movement accumulated this frame.
		[[nodiscard]] glm::vec2 wheel() const { return m_wheel; }

		/// Shorthand to see the `position()` x component
		[[nodiscard]] float x() const { return m_position.x; }

		/// Shorthand to see the `position()` y component
		[[nodiscard]] float y() const { return m_position.y; }

		/// Whether the given MouseButton is currently held down
		[[nodiscard]] bool down(MouseButton btn) const { return m_down[index(btn)]; }

		/// Whether the given MouseButton was pressed this frame
		[[nodiscard]] bool pressed(MouseButton btn) const { return m_pressed[index(btn)]; }

		/// Whether the given MouseButton was released this frame
		[[nodiscard]] bool released(MouseButton btn) const { return m_released[index(btn)]; }

		/// Returns the timestamp of when the MouseButton was last down
		[[nodiscard]] u64 timestamp(MouseButton btn) const { return m_timestamps[index(btn)]; }

		/// Returns the timestamp of the last mouse motion
		[[nodiscard]] u64 timestamp_motion() const { return m_motion_timestamp; }

	private:
		friend class Input;

		static u32 index(MouseButton btn)
		{
			const auto i = static_cast<u32>(btn);
			EMBER_ASSERT(i < MAX_MOUSE_BUTTONS);
			return i;
		}

		void on_button(MouseButton btn, bool down, u64 timestamp)
		{
			const u32 i = index(btn);

			if (down)
			{
				m_down[i]		= true;
				m_pressed[i]	= true;
				m_timestamps[i] = timestamp;
			}
			else
			{
				m_down[i]	  = false;
				m_released[i] = true;
			}
		}

		void on_move(glm::vec2 pos, glm::vec2 delta, u64 timestamp)
		{
			m_position			= pos;
			m_delta			   += delta;
			m_motion_timestamp	= timestamp;
		}

		void on_wheel(glm::vec2 wheel) { m_wheel += wheel; }

		void reset()
		{
			memset(m_pressed, 0, sizeof(m_pressed));
			memset(m_released, 0, sizeof(m_released));
			m_delta = {};
			m_wheel = {};
		}

		/// whether a button was pressed this frame
		bool m_pressed[MAX_MOUSE_BUTTONS] = {false};

		/// whether a button was held this frame
		bool m_down[MAX_MOUSE_BUTTONS] = {false};

		/// whether a button was released this frame
		bool m_released[MAX_MOUSE_BUTTONS] = {false};

		/// timestamp of when a button was last pressed
		u64 m_timestamps[MAX_MOUSE_BUTTONS] = {false};

		/// timestamp of when the last mouse motion was
		u64 m_motion_timestamp = 0;

		/// mouse position (screen coordinates)
		glm::vec2 m_screen_position = {0, 0};

		/// mouse position (window coordinates);
		glm::vec2 m_position = {0, 0};

		/// mouse wheel value for current frame
		glm::vec2 m_wheel = {0, 0};

		/// mouse delta value compared to previous position
		glm::vec2 m_delta = {0, 0};
	};
}
