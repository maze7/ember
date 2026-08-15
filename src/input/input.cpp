#include <ember/core/profile.h>
#include <ember/input/input.h>

namespace ember
{
	void Input::step_state()
	{
		m_prev_state = m_state;
		m_state.keyboard.reset();
		m_state.mouse.reset();

		for (Controller& controller : m_state.controllers)
			controller.reset();
	}


	void Input::update(std::span<const InputEvent> events)
	{
		EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_INPUT);
	}
}
