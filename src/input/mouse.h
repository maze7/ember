#pragma once

#include "buttons.h"
#include "glm.hpp"

namespace Ember {
    class Input;
    class Window;

    class Mouse {
    public:
        static constexpr u32 MAX_MOUSE_BUTTONS = 8;

        /// Mouse position, relative to the window, in Pixel coordinates.
        [[nodiscard]] glm::vec2 position() const { return m_position; }

        /// Delta to the previous mouse position, in Pixels coordinates.
        [[nodiscard]] glm::vec2 delta() const { return m_delta; }

        /// Shorthand to see the `position()` x component.
        [[nodiscard]] float x() const { return m_position.x; }

        /// Shorthand to see the `position()` y component.
        [[nodiscard]] float y() const { return m_position.y; }

        /// The Mouse Wheel value
        [[nodiscard]] glm::vec2 wheel() const { return m_wheel; }

        [[nodiscard]] bool down(MouseButton button) const;

        [[nodiscard]] bool pressed(MouseButton button) const;

        [[nodiscard]] bool released(MouseButton button) const;

        [[nodiscard]] u64 button_timestamp(MouseButton button) const;

        [[nodiscard]] u64 motion_timestamp() const;
    private:
        friend class Window;
        friend class Input;

        void on_button(MouseButton button, bool down);

        void on_move(const glm::vec2& position, const glm::vec2& delta);

        void on_wheel(const glm::vec2& wheel);

        void reset();

        /// whether a button was pressed this frame
        bool m_pressed[MAX_MOUSE_BUTTONS];

        /// whether a button was held this frame
        bool m_down[MAX_MOUSE_BUTTONS];

        /// whether a button was released this frame
        bool m_released[MAX_MOUSE_BUTTONS];

        /// timestamp of when a button was last pressed
        u64 m_timestamps[MAX_MOUSE_BUTTONS];

        /// mouse position (screen coordinates)
        glm::vec2 m_screen_position;

        /// mouse position (window coordinates);
        glm::vec2 m_position;

        /// mouse wheel value for current frame
        glm::vec2 m_wheel;

        /// mouse delta value compared to previous position
        glm::vec2 m_delta;
    };
} // namespace Ember
