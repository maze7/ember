#include "platform/window.h"

#include <algorithm>
#include <vector>

#include "SDL3/SDL.h"
#include "core/common.h"
#include "core/logger.h"

using namespace Ember;

static constexpr u32 SDL_SUBSYSTEMS = SDL_INIT_GAMEPAD | SDL_INIT_VIDEO | SDL_INIT_EVENTS;

u8 Window::s_num_windows = 0;

namespace
{
	struct Joystick
	{
		u32 instance_id;
		SDL_Joystick* ptr;
	};
	struct Gamepad
	{
		u32 instance_id;
		SDL_Gamepad* ptr;
	};

	std::vector<Joystick> joysticks;
	std::vector<Gamepad> gamepads;

	/**
	 * @brief Normalises a 16-bit signed axis value to a float between -1.0
	 * and 1.0. This uses the full range of a signed Sint16.
	 */
	float normalize_axis_value(i16 value) {
		return value >= 0 ? static_cast<float>(value) / 32767.0f : static_cast<float>(value) / 32768.0f;
	}

	/**
	 * @brief Finds the first available (disconnected) controller slot.
	 * @return A pointer to the free controller, or nullptr if none are
	 * available.
	 */
	Controller* next_free_controller_slot(InputState& state) {
		for(auto& controller : state.controllers) {
			if(!controller.is_connected())
				return &controller;
		}

		return nullptr;
	}
} // namespace

Window::Window(const char* title, i32 width, i32 height) {
	// If this is the first window, initialize the required SDL subsystems.
	if(s_num_windows == 0) {
		if(!SDL_Init(SDL_SUBSYSTEMS)) {
			EMBER_ERROR("SDL_Init failed: {}", SDL_GetError());
		}

		const int compiled = SDL_VERSION;
		const int linked = SDL_GetVersion();
		EMBER_INFO("SDL compiled version: {}.{}.{}", SDL_VERSIONNUM_MAJOR(compiled), SDL_VERSIONNUM_MINOR(compiled), SDL_VERSIONNUM_MICRO(compiled));
		EMBER_INFO("SDL linked version: {}.{}.{}", SDL_VERSIONNUM_MAJOR(linked), SDL_VERSIONNUM_MINOR(linked), SDL_VERSIONNUM_MICRO(linked));
	}

	auto window_flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
#ifdef EMBER_PLATFORM_STEAM_DECK
	window_flags |= SDL_WINDOW_FULLSCREEN;
#else
	window_flags |= SDL_WINDOW_RESIZABLE;
#endif

	m_window = SDL_CreateWindow(title, width, height, window_flags);
	s_num_windows++;
}

Window::~Window() {
	if(m_window)
		SDL_DestroyWindow(m_window);

	s_num_windows--;

	// If this was the last window, shut down SDL.
	if(s_num_windows == 0) {
		// cleanup gamepads & joysticks
		std::for_each(joysticks.begin(), joysticks.end(), [](Joystick& j) { SDL_CloseJoystick(j.ptr); });
		std::for_each(gamepads.begin(), gamepads.end(), [](Gamepad& g) { SDL_CloseGamepad(g.ptr); });

		joysticks.clear();
		gamepads.clear();

		SDL_Quit();
	}
}

// --- Getters ---
i32 Window::width() const {
	return size().x;
}
i32 Window::height() const {
	return size().y;
}

glm::ivec2 Window::size() const {
	glm::ivec2 size;
	SDL_GetWindowSize(m_window, &size.x, &size.y);

	return size;
}

glm::ivec2 Window::drawable_size() const {
	auto scale = pixel_density();
	return { width() * scale, height() * scale };
}

float Window::pixel_density() const {
	return SDL_GetWindowPixelDensity(m_window);
}

float Window::aspect_ratio() const {
	const auto sz = size();

	// avoid division by zero if height is 0.
	return (sz.y > 0) ? static_cast<float>(sz.x) / static_cast<float>(sz.y) : 0.0f;
}

const char* Window::title() const {
	return SDL_GetWindowTitle(m_window);
}

// --- Setters ---
void Window::set_size(i32 width, i32 height) {
	SDL_SetWindowSize(m_window, width, height);
}

void Window::set_title(const char* title) {
	SDL_SetWindowTitle(m_window, title);
}

void Window::set_fullscreen(bool flag) {
	SDL_SetWindowFullscreen(m_window, flag);
}

void Window::set_visible(bool flag) {
	if (flag)
		SDL_ShowWindow(m_window);
	else
		SDL_HideWindow(m_window);
}

void Window::set_text_input(bool flag) {
	if (flag)
		SDL_StartTextInput(m_window);
	else
		SDL_StopTextInput(m_window);
}

bool Window::poll_events(InputState& state) {
	// Update mouse position once per frame, before processing the event queue.
	update_mouse_position(state);

	// Process event queue for frame.
	SDL_Event event;
	while(SDL_PollEvent(&event)) {
		switch(event.type) {
		case SDL_EVENT_QUIT:
			return false;

		// Keyboard Events
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			on_keyboard_event(event.key, state);
			break;
		case SDL_EVENT_TEXT_INPUT:
			on_keyboard_text_input(event.text, state);

		// Mouse Events
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			on_mouse_button_event(event.button, state);
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			state.mouse.on_wheel({event.wheel.x, event.wheel.y});
			break;

		// Joystick Events (for non-gamepad devices)
		case SDL_EVENT_JOYSTICK_ADDED:
			on_joystick_added(event.jdevice, state);
			break;
		case SDL_EVENT_JOYSTICK_REMOVED:
			on_joystick_removed(event.jdevice, state);
			break;
		case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
		case SDL_EVENT_JOYSTICK_BUTTON_UP:
			on_joystick_button(event.jbutton, state);
			break;
		case SDL_EVENT_JOYSTICK_AXIS_MOTION:
			on_joystick_axis(event.jaxis, state);
			break;

		// Gamepad Events (preferred over joystick)
		case SDL_EVENT_GAMEPAD_ADDED:
			on_gamepad_added(event.gdevice, state);
			break;
		case SDL_EVENT_GAMEPAD_REMOVED:
			on_gamepad_removed(event.gdevice, state);
			break;
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
			on_gamepad_button(event.gbutton, state);
			break;
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			on_gamepad_axis(event.gaxis, state);
			break;

		default:
			break;
		}
	}

	return true;
}

// --- Private Event Handlers ---
void Window::update_mouse_position(InputState& state) {
	int win_x, win_y;
	float global_x, global_y;

	SDL_GetWindowPosition(m_window, &win_x, &win_y);
	SDL_GetGlobalMouseState(&global_x, &global_y);

	// Calculate window-relative and global mouse positions.
	state.mouse.on_move({global_x - win_x, global_y - win_y}, {global_x, global_y});
}

void Window::on_keyboard_event(const SDL_KeyboardEvent& event, InputState& state) {
	// Ignore key repeat events for down/up state changes.
	if(!event.repeat) {
		state.keyboard.on_key(static_cast<Key>(event.scancode), event.type == SDL_EVENT_KEY_DOWN);
	}
}

void Window::on_keyboard_text_input(const SDL_TextInputEvent& event, InputState& state) {
	state.keyboard.text += event.text;
}

void Window::on_mouse_button_event(const SDL_MouseButtonEvent& event, InputState& state) {
	MouseButton btn = MouseButton::None;
	switch(event.button) {
	case SDL_BUTTON_LEFT:
		btn = MouseButton::Left;
		break;
	case SDL_BUTTON_RIGHT:
		btn = MouseButton::Right;
		break;
	case SDL_BUTTON_MIDDLE:
		btn = MouseButton::Middle;
		break;
	default:
		return; // Do nothing for other buttons (X1, X2, etc.)
	}
	state.mouse.on_button(btn, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
}

// --- Joystick Specific Handlers ---

void Window::on_joystick_added(const SDL_JoyDeviceEvent& event, InputState& state) {
	// An SDL_Gamepad is also an SDL_Joystick. We prefer the Gamepad API, so
	// ignore this event if the device is a recognized gamepad. A GAMEPAD_ADDED
	// event will be sent for it.
	if(SDL_IsGamepad(event.which)) {
		return;
	}

	if(Controller* controller = next_free_controller_slot(state)) {
		SDL_Joystick* ptr = SDL_OpenJoystick(event.which);
		if(!ptr)
			return;

		joysticks.emplace_back(event.which, ptr);
		controller->connect(event.which, SDL_GetJoystickName(ptr), false, SDL_GetNumJoystickButtons(ptr), SDL_GetNumJoystickAxes(ptr), SDL_GetJoystickVendor(ptr),
		                    SDL_GetJoystickProduct(ptr), SDL_GetJoystickProductVersion(ptr));
	}
}

void Window::on_joystick_removed(const SDL_JoyDeviceEvent& event, InputState& state) {
	if(SDL_IsGamepad(event.which))
		return;

	if(auto* controller = state.controller(event.which)) {
		controller->disconnect();
	}

	std::erase_if(joysticks, [id = event.which](const Joystick& joystick) {
		if(joystick.instance_id == id) {
			SDL_CloseJoystick(joystick.ptr);
			return true;
		}
		return false;
	});
}

void Window::on_joystick_button(const SDL_JoyButtonEvent& event, InputState& state) {
	if(SDL_IsGamepad(event.which))
		return;

	if(auto* controller = state.controller(event.which)) {
		controller->on_button(static_cast<ControllerButton>(event.button), event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
	}
}

void Window::on_joystick_axis(const SDL_JoyAxisEvent& event, InputState& state) {
	if(SDL_IsGamepad(event.which))
		return;

	if(auto* controller = state.controller(event.which)) {
		controller->on_axis(static_cast<Axis>(event.axis), normalize_axis_value(event.value));
	}
}

// --- Gamepad Specific Handlers ---

void Window::on_gamepad_added(const SDL_GamepadDeviceEvent& event, InputState& state) {
	if(Controller* controller = next_free_controller_slot(state)) {
		SDL_Gamepad* ptr = SDL_OpenGamepad(event.which);
		if(!ptr)
			return;

		gamepads.emplace_back(event.which, ptr);
		controller->connect(event.which, SDL_GetGamepadName(ptr), true,
		                    15, // Gamepads have a fixed button/axis count
		                    6, SDL_GetGamepadVendor(ptr), SDL_GetGamepadProduct(ptr), SDL_GetGamepadProductVersion(ptr));
	}
}

void Window::on_gamepad_removed(const SDL_GamepadDeviceEvent& event, InputState& state) {
	if(auto* controller = state.controller(event.which)) {
		controller->disconnect();
	}

	std::erase_if(gamepads, [id = event.which](const Gamepad& gamepad) {
		if(gamepad.instance_id == id) {
			SDL_CloseGamepad(gamepad.ptr);
			return true;
		}
		return false;
	});
}

void Window::on_gamepad_button(const SDL_GamepadButtonEvent& event, InputState& state) {
	if(auto* controller = state.controller(event.which)) {
		controller->on_button(static_cast<ControllerButton>(event.button), event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
	}
}

void Window::on_gamepad_axis(const SDL_GamepadAxisEvent& event, InputState& state) {
	if(auto* controller = state.controller(event.which)) {
		controller->on_axis(static_cast<Axis>(event.axis), normalize_axis_value(event.value));
	}
}
