#include "input/virtual_stick.h"

#include <gtx/norm.hpp>

using namespace Ember;

VirtualStick::VirtualStick(Input& input, VirtualAxis::Overlap overlap_behavior, int device, float deadzone)
    : m_input(input), m_xaxis(input, device), m_yaxis(input, device), deadzone(deadzone) {
	m_xaxis.overlap = overlap_behavior;
	m_yaxis.overlap = overlap_behavior;
}

glm::vec2 VirtualStick::value() const {
	glm::vec2 v = {m_xaxis.value(), m_yaxis.value()};

	// return no value if magnitude has not exceeded deadzone
	if(deadzone != 0 && length2(v) < (deadzone * deadzone)) {
		return glm::vec2(0);
	}

	return v;
}

glm::vec2 VirtualStick::value_normalized() const {
	auto raw = value();

	const float angle = atan2(raw.y, raw.x);
	const float step = std::numbers::pi_v<float> / 4.0f;
	const float snapped = round(angle / step) * step;

	return glm::normalize(glm::vec2(cos(snapped), sin(snapped)));
}

VirtualStick& VirtualStick::add(Key left, Key right, Key up, Key down) {
	m_xaxis.add(left, right);
	m_yaxis.add(up, down);
	return *this;
}

VirtualStick& VirtualStick::add(ControllerButton left, ControllerButton right, ControllerButton up, ControllerButton down) {
	m_xaxis.add(left, right);
	m_yaxis.add(up, down);
	return *this;
}

VirtualStick& VirtualStick::add(Axis x, Axis y, float x_deadzone, float y_deadzone) {
	m_xaxis.add(x, x_deadzone);
	m_yaxis.add(y, y_deadzone);
	return *this;
}

VirtualStick& VirtualStick::add_arrow_keys() {
	return add(Key::Left, Key::Right, Key::Up, Key::Down);
}

VirtualStick& VirtualStick::add_wasd() {
	return add(Key::A, Key::D, Key::W, Key::S);
}

VirtualStick& VirtualStick::add_left_joystick(float x_deadzone, float y_deadzone) {
	return add(Axis::LeftX, Axis::LeftY, x_deadzone, y_deadzone);
}

VirtualStick& VirtualStick::add_right_joystick(float x_deadzone, float y_deadzone) {
	return add(Axis::RightX, Axis::RightY, x_deadzone, y_deadzone);
}

VirtualStick& VirtualStick::add_dpad() {
	return add(ControllerButton::Left, ControllerButton::Right, ControllerButton::Up, ControllerButton::Down);
}
