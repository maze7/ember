#include "input/bindings.h"
#include "input/input.h"
#include "math/math.h"

using namespace Ember;

BindingState MouseButtonBinding::get_state(Input& input, int device) {
	return {
	    .pressed = input.mouse().pressed(button),
	    .released = input.mouse().released(button),
	    .down = input.mouse().down(button),
	    .value = input.mouse().down(button) ? 1.f : 0.f,
	    .value_no_deadzone = input.mouse().down(button) ? 1.f : 0.f,
	    //.timestamp = input.mouse().button_timestamp(button),
	};
}

BindingState MouseMotionBinding::get_state(Input& input, int device) {
	return {
	    .pressed = get_value(input.state()) > 0 && get_value(input.prev_state()) <= 0,
	    .released = get_value(input.state()) <= 0 && get_value(input.prev_state()) > 0,
	    .down = get_value(input.state()) > 0,
	    .value = get_value(input.state()),
	    // .timestamp = input.mouse().motion_timestamp(),
	};
}

float MouseMotionBinding::get_value(InputState& state) {
	auto value = glm::dot(axis, state.mouse.delta());
	return clamped_map(value, sign * min, sign * max, 0, 1);
}

BindingState KeyboardKeyBinding::get_state(Input& input, int device) {
	return {
	    .pressed = input.keyboard().pressed(key),
	    .released = input.keyboard().released(key),
	    .down = input.keyboard().down(key),
	    .value = input.keyboard().down(key) ? 1.0f : 0.0f,
	    // .timestamp = input.keyboard().timestamp(key),
	};
}

BindingState ControllerButtonBinding::get_state(Input& input, int device) {
	return {
	    .pressed = input.controller(device).pressed(button),
	    .released = input.controller(device).released(button),
	    .down = input.controller(device).down(button),
	    .value = input.controller(device).down(button) ? 1.f : 0.f,
	    // .timestamp = input.controller(device).timestamp(button),
	};
}

BindingState ControllerAxisBinding::get_state(Input& input, int device) {
	auto value = get_value(input.state(), device, deadzone);
	auto prev = get_value(input.prev_state(), device, deadzone);

	return {
	    .pressed = value > 0 && prev <= 0, .released = value <= 0 && prev > 0, .down = value > 0, .value = value,
	    //.timestamp = input.controller(device).timestamp(axis)
	};
}

float ControllerAxisBinding::get_value(InputState& state, int device, float deadzone) {
	auto val = state.controllers[device].axis(axis);
	return clamped_map(val, sign * deadzone, sign);
}
