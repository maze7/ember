#include "input/virtual_axis.h"

using namespace Ember;

VirtualAxis::VirtualAxis(Input& input, int controller) : m_input(input), m_negative(input, controller), m_positive(input, controller) {}

VirtualAxis::~VirtualAxis() {}

VirtualAxis& VirtualAxis::add(Key negative, Key positive) {
	m_negative.add({negative});
	m_positive.add({positive});
	return *this;
}

VirtualAxis& VirtualAxis::add(MouseButton negative, MouseButton positive) {
	m_negative.add({negative});
	m_positive.add({positive});
	return *this;
}

VirtualAxis& VirtualAxis::add(ControllerButton negative, ControllerButton positive) {
	m_negative.add({negative});
	m_positive.add({positive});
	return *this;
}

VirtualAxis& VirtualAxis::add(Axis axis, float deadzone) {
	m_negative.add(axis, -1, deadzone);
	m_positive.add(axis, +1, deadzone);
	return *this;
}

float VirtualAxis::value() const {
	auto neg = m_negative.state();
	auto pos = m_positive.state();

	switch(overlap) {
	case Overlap::CancelOut:
		return clamp(pos.value - neg.value, -1, 1);
	case Overlap::TakeNewer:
		if(pos.down && neg.down)
			return neg.timestamp > pos.timestamp ? -neg.value : pos.value;
		if(pos.down)
			return pos.value;
		if(neg.down)
			return -neg.value;
	case Overlap::TakeOlder:
		if(pos.down && neg.down)
			return neg.timestamp < pos.timestamp ? -neg.value : pos.value;
		if(pos.down)
			return pos.value;
		if(neg.down)
			return -neg.value;
	}

	return 0;
}
