#pragma once

#include "SDL3/SDL_events.h"
#include "core/common.h"
#include "ext/vector_int2.hpp"
#include "input/input.h"

// forward declaration of SDL types
struct SDL_Window;

namespace Ember
{
	class Window
	{
	public:
		Window(const char* title, i32 width, i32 height);
		~Window();

		// -- Getters --
		[[nodiscard]] i32 width() const;
		[[nodiscard]] i32 height() const;
		[[nodiscard]] glm::ivec2 size() const;
		[[nodiscard]] glm::ivec2 drawable_size() const;
		[[nodiscard]] float pixel_density() const;
		[[nodiscard]] float aspect_ratio() const;
		[[nodiscard]] const char* title() const;
		[[nodiscard]] SDL_Window* native_handle() const {
			return m_window;
		}

		// -- Setters --
		void set_size(i32 width, i32 height);
		void set_title(const char* title);
		void set_fullscreen(bool flag);
		void set_visible(bool flag);
		void set_text_input(bool flag);

		// -- Event Handling --
		bool poll_events(InputState& state);

	private:
		void update_mouse_position(InputState& state);
		void on_keyboard_event(const SDL_KeyboardEvent& event, InputState& state);
		void on_keyboard_text_input(const SDL_TextInputEvent& event, InputState& state);
		void on_mouse_button_event(const SDL_MouseButtonEvent& event, InputState& state);
		void on_joystick_added(const SDL_JoyDeviceEvent& event, InputState& state);
		void on_joystick_removed(const SDL_JoyDeviceEvent& event, InputState& state);
		void on_joystick_button(const SDL_JoyButtonEvent& event, InputState& state);
		void on_joystick_axis(const SDL_JoyAxisEvent& event, InputState& state);
		void on_gamepad_added(const SDL_GamepadDeviceEvent& event, InputState& state);
		void on_gamepad_removed(const SDL_GamepadDeviceEvent& event, InputState& state);
		void on_gamepad_button(const SDL_GamepadButtonEvent& event, InputState& state);
		void on_gamepad_axis(const SDL_GamepadAxisEvent& event, InputState& state);

		static u8 s_num_windows;
		SDL_Window* m_window;
	};
}
