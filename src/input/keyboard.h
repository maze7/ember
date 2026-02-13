#pragma once

#include "keys.h"

namespace Ember {
    class Input;
    class Keyboard {
    public:
        static constexpr u32 MAX_KEYS = 512;

        // returns true if the given key was pressed during the current frame
        bool pressed(Key key) const;

        // returns true if the given key is currently held down
        bool down(Key key) const;

        // returns true if the given key was released during the current frame
        bool released(Key key) const;

        // returns true if either of the CTRL keys are pressed on the keyboard
        bool ctrl() const;

        // returns true if either of the SHIFT keys are pressed on the keyboard
        bool shift() const;

        // returns true if either of the ALT keys are pressed on the keyboard
        bool alt() const;

        // called when the platform layer detects a key press
        void on_key(Key key, bool down);

        // reset internal state to default
        void reset();

        // text that has been entered via the keyboard
        std::string text;
    private:
        friend class Input;

        // whether a key was pressed in the current frame
        bool m_pressed[MAX_KEYS];

        // whether a key is currently held
        bool m_down[MAX_KEYS];

        // whether a key was released this frame
        bool m_released[MAX_KEYS];

        // the timestamp for each key press
        u64 m_timestamp[MAX_KEYS];
    };
} // namespace Ember
