#include "input/controller.h"
#include "core/common.h"
#include "math/math.h"

#include <SDL3/SDL_gamepad.h>

using namespace Ember;

namespace
{
	Controller empty_controller;
}

bool Controller::down(ControllerButton button) const {
	u32 index = (u32)button;

	EMBER_ASSERT(index < MAX_BUTTONS);
	return m_down[index];
}

bool Controller::pressed(ControllerButton button) const {
	u32 index = (u32)button;

	EMBER_ASSERT(index < MAX_BUTTONS);
	return m_pressed[index];
}

bool Controller::released(ControllerButton button) const {
	u32 index = (u32)button;

	EMBER_ASSERT(index < MAX_BUTTONS);
	return m_released[index];
}

float Controller::axis(Axis axis) const {
	u32 index = (u32)axis;

	EMBER_ASSERT(index < MAX_AXES);
	return m_axis[index];
}

void Controller::rumble(float intensity, float duration) {
	rumble(intensity, intensity, duration);
}

void Controller::rumble(float low_intensity, float high_intensity, float duration) {
	if(!m_connected)
		return;

	u16 high_freq = clamp(high_intensity, 0, 1) * 0xFFFF;
	u16 low_freq = clamp(low_intensity, 0, 1) * 0xFFFF;
	u32 duration_ms = (u32)duration * 1000;

	if(m_is_gamepad) {
		auto* ptr = SDL_GetGamepadFromID(m_id);
		if(ptr) {
			SDL_RumbleGamepad(ptr, low_freq, high_freq, duration_ms);
		}
	} else {
		auto* ptr = SDL_GetJoystickFromID(m_id);
		if(ptr) {
			SDL_RumbleJoystick(ptr, low_freq, high_freq, duration_ms);
		}
	}
}

void Controller::connect(u32 id, const std::string& name, bool is_gamepad, u8 button_count, u8 axis_count, u16 vendor, u16 product, u16 version) {
	*this = empty_controller;
	m_id = id;
	m_name = name;
	m_connected = true;
	m_is_gamepad = is_gamepad;
	m_button_count = button_count;
	m_axis_count = axis_count;
	m_vendor_id = vendor;
	m_product_id = product;
	m_product_version = version;

	Log::info("Controller {} connected ({})", m_id, name);
}

void Controller::disconnect() {
	*this = empty_controller;
}

void Controller::reset() {
	std::ranges::fill(m_pressed, false);
	std::ranges::fill(m_released, false);
}

void Controller::on_button(ControllerButton button, bool down) {
	u32 index = (u32)button;

	EMBER_ASSERT(index < MAX_BUTTONS);

	if(down) {
		m_down[index] = true;
		m_pressed[index] = true;
	} else {
		m_down[index] = false;
		m_released[index] = true;
	}
}

void Controller::on_axis(Axis axis, float value) {
	EMBER_ASSERT((u32)axis < MAX_BUTTONS);
	m_axis[(u32)axis] = value;
}
