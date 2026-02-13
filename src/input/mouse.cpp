#include "input/mouse.h"

#include "core/time.h"

using namespace Ember;

void Mouse::on_move(const glm::vec2& position, const glm::vec2& screen_position) {
	m_position = position;
	m_screen_position = screen_position;
}

void Mouse::on_wheel(const glm::vec2& wheel) {
	m_wheel = wheel;
}

void Mouse::reset() {
	std::ranges::fill(m_pressed, false);
	std::ranges::fill(m_released, false);
	m_wheel = glm::vec2(0, 0);
}

bool Mouse::down(MouseButton button) const {
	EMBER_ASSERT((u32)button < MAX_MOUSE_BUTTONS);
	return m_down[(u32)button];
}

bool Mouse::pressed(MouseButton button) const {
	EMBER_ASSERT((u32)button < MAX_MOUSE_BUTTONS);
	return m_pressed[(u32)button];
}

bool Mouse::released(MouseButton button) const {
	EMBER_ASSERT((u32)button < MAX_MOUSE_BUTTONS);
	return m_released[(u32)button];
}

void Mouse::on_button(MouseButton button, bool down) {
	const u32 index = (u32)button;
	EMBER_ASSERT(index < MAX_MOUSE_BUTTONS);

	if(down) {
		m_pressed[index] = true;
		m_down[index] = true;
		m_timestamps[index] = Time::ticks();
	} else {
		m_released[index] = true;
		m_down[index] = false;
	}
}
