#include <SDL/SDL.h>
#include <ember/platform/platform.h>
#include <ember/platform/window.h>

#include <string_view>

namespace ember
{
	struct Platform::Impl
	{
	};

	PumpResult Platform::pump_events(Input& input) noexcept
	{
		EMBER_ASSERT(m_impl != nullptr);
		EMBER_ASSERT(m_impl->is_owner_thread());

		PumpResult result{};

		SDL_PumpEvents();
		SDL_Event event{};

		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_POLL_SENTINEL)
				break;

			switch (event.type)
			{
				case SDL_EVENT_QUIT:
					result.quit_requested = true;
					break;

				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_KEY_UP:
				{
					input.on_key(
						m_impl->key_from_sdl(event.key.scancode),
						event.key.down,
						event.key.repeat,
						event.key.timestamp);
					break;
				}

				case SDL_EVENT_TEXT_INPUT:
				{
					WindowHandle window = m_impl->window_from_sdl(event.text.windowID);

					if (event.text.text != nullptr)
						input.on_text(std::string_view{event.text.text}, window);
				}

				case SDL_EVENT_TEXT_EDITING:
				{
					WindowHandle window = m_impl->window_from_sdl(event.edit.windowID);

					input.on_composition(
						event.edit.text != nullptr ? std::string_view{event.edit.text} : std::string_view{},
						event.edit.start,
						event.edit.length,
						window);
					break;
				}

				case SDL_EVENT_MOUSE_MOTION:
				{
					// Ignore mouse events synthesized from touch or pen input.
					if (event.motion.which == SDL_TOUCH_MOUSEID || event.motion.which == SDL_PEN_MOUSEID)
						break;

					WindowHandle window = m_impl->window_from_sdl(event.motion.windowID);

					input.on_mouse_motion(
						{event.motion.x, event.motion.y},
						{event.motion.xrel, event.motion.yrel},
						window,
						event.motion.timestamp);
					break;
				}

				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:
				{
					std::optional<MouseButton> button = m_impl->mouse_button_from_sdl(event.button.button);

					if (!button)
						break;

					WindowHandle window = m_impl->window_from_sdl(event.button.windowID);
					input.on_mouse_button(*button, event.button.down, window, event.button.timestamp);

					break;
				}

				case SDL_EVENT_MOUSE_WHEEL:
				{
					WindowHandle window = m_impl->window_from_sdl(event.wheel.windowID);
					f32 direction		= event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;

					input.on_mouse_wheel(
						{
							event.wheel.x * direction,
							event.wheel.y * direction,
						},
						window,
						event.wheel.timestamp);
					break;
				}

				case SDL_EVENT_GAMEPAD_ADDED:
					m_impl->open_gamepad(input, GamepadId{event.gdevice.which}, event.gdevice.timestamp);
					break;

				case SDL_EVENT_GAMEPAD_REMOVED:
					m_impl->close_gamepad(input, GamepadId{event.gdevice.which}, event.gdevice.timestamp);
					break;

				case SDL_EVENT_GAMEPAD_REMAPPED:
					m_impl->remap_gamepad(input, GamepadId{event.gdevice.which});
					break;

				case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				case SDL_EVENT_GAMEPAD_BUTTON_UP:
				{
					if (event.gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT)
						break;

					input.on_gamepad_button(
						GamepadId{event.gbutton.which},
						static_cast<GamepadButton>(event.gbutton.button),
						event.gbutton.down,
						event.gbutton.timestamp);
					break;
				}

				case SDL_EVENT_GAMEPAD_AXIS_MOTION:
				{
					if (event.gaxis.axis >= SDL_GAMEPAD_AXIS_COUNT)
						break;

					const f32 value = event.gaxis.value >= 0 ? static_cast<f32>(event.gaxis.value) / 32767.0f
															 : static_cast<f32>(event.gaxis.value) / 32768.0f;

					input.on_gamepad_axis(
						GamepadId{event.gaxis.which},
						static_cast<GamepadAxis>(event.gaxis.axis),
						value,
						event.gaxis.timestamp);
					break;
				}

				case SDL_EVENT_WINDOW_FOCUS_LOST:
				{
					const WindowHandle window = m_impl->window_from_sdl(event.window.windowID);

					input.on_focus_lost(window, event.window.timestamp);

					m_impl->handle_window_event(event.window);
					break;
				}

				case SDL_EVENT_WINDOW_FOCUS_GAINED:
				case SDL_EVENT_WINDOW_MOUSE_ENTER:
				case SDL_EVENT_WINDOW_MOUSE_LEAVE:
				case SDL_EVENT_WINDOW_MOVED:
				case SDL_EVENT_WINDOW_RESIZED:
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
				case SDL_EVENT_WINDOW_RESTORED:
				case SDL_EVENT_WINDOW_MAXIMIZED:
				case SDL_EVENT_WINDOW_MINIMIZED:
				case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
				case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
				case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
					m_impl->handle_window_event(event.window);
					break;

				default:
					break;
			}
		}

		// Poll first, publish second. New input is visible immediately.
		input.publish(SDL_GetTicksNS());

		return result;
	}
}
