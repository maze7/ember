#include "input/virtual_input.h"

using namespace Ember;

VirtualInput::VirtualInput(Input& input, int controller, float buffer) : m_input(input), m_device(controller), m_buffer(buffer) {
	m_input.add_virtual_input(this);
}

VirtualInput::~VirtualInput() {
	m_input.remove_virtual_input(this);
}
