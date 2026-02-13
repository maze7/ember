#include "input/input.h"
#include "input/virtual_input.h"

using namespace Ember;

Controller* InputState::controller(u32 id) {
	for(int i = 0; i < MAX_CONTROLLERS; i++) {
		if(controllers[i].id() == id)
			return &controllers[i];
	}

	return nullptr;
}

void Input::step_state() {
	// update virtual buttons
	for(auto& btn : m_virtual_inputs)
		btn->update();

	// cycle states
	m_prev_state = m_state;

	// reset current state
	m_state.keyboard.reset();
	m_state.mouse.reset();

	// reset controller state
	for(auto& controller : m_state.controllers)
		controller.reset();
}

void Input::add_virtual_input(VirtualInput* input) {
	m_virtual_inputs.push_back(input);
}

void Input::remove_virtual_input(VirtualInput* input) {
	std::erase(m_virtual_inputs, input);
}
