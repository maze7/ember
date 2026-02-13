#pragma once

#include "controller.h"
#include "keyboard.h"
#include "mouse.h"
#include <vector>

namespace Ember
{
	static constexpr u32 MAX_CONTROLLERS = 4;

	// Forward Declarations
	class VirtualInput;
	class VirtualAxis;
	class VirtualStick;

	/// Stores the state of all Input devices for a given frame
	struct InputState
	{
		Controller controllers[MAX_CONTROLLERS];
		Keyboard keyboard;
		Mouse mouse;

		Controller* controller(u32 id);
	};

	/// Primary interface used to access all Input Devices across frames
	class Input
	{
	public:
		/**
		 * Returns a reference to the current InputState
		 * @return InputState of current frame
		 */
		InputState& state() {
			return m_state;
		}

		/**
		 * Returns a reference to the previous InputState
		 * @return InputState of previous frame
		 */
		InputState& prev_state() {
			return m_prev_state;
		}

		/**
		 * Cycles the InputState forward by one frame.
		 * Sets the previous InputState to the current InputState and then
		 * resets the current InputState to empty.
		 */
		void step_state();

		/**
		 * Accesses the Mouse wrapper for the current frame.
		 * @return Mouse for current frame
		 */
		Mouse& mouse() {
			return m_state.mouse;
		}

		/**
		 * Accesses the Mouse wrapper for the previous frame.
		 * @return Mouse for the previous frame
		 */
		Mouse& prev_mouse() {
			return m_prev_state.mouse;
		}

		/**
		 * Accesses the Keyboard wrapper for the current frame.
		 * @return Keyboard for the current frame.
		 */
		Keyboard& keyboard() {
			return m_state.keyboard;
		}

		/**
		 * Accesses the Keyboard wrapper for the previous frame.
		 * @return Keyboard for the previous frame.
		 */
		Keyboard& prev_keyboard() {
			return m_prev_state.keyboard;
		}

		/**
		 * Accesses a given Controller from the current InputState by its index.
		 * @param index Index of desired controller
		 * @return Reference to desired Controller
		 */
		Controller& controller(const u32 index) {
			EMBER_ASSERT(index < MAX_CONTROLLERS);
			return m_state.controllers[index];
		}

		/**
		 * Accesses a given Controller from previous InputState by its index.
		 * @param index Index of the desired controller
		 * @return Reference to desired Controller
		 */
		Controller& prev_controller(const u32 index) {
			EMBER_ASSERT(index < MAX_CONTROLLERS);
			return m_state.controllers[index];
		}

		/**
		 * Adds a VirtualInput to the list of virtual inputs to be tracked. The
		 * Input class will automatically call `VirtualInput::update()` each
		 * frame, until it is deregistered.
		 * @param input VirtualInput to be tracked
		 * @note stores a non-owning pointer to the VirtualInput
		 */
		void add_virtual_input(VirtualInput* input);

		/**
		 * Removes a VirtualInput from the list of virtual inputs to be tracked.
		 * This is usually called by the VirtualInput's own destructor
		 * @param input VirtualInput to be removed
		 */
		void remove_virtual_input(VirtualInput* input);

	private:
		/// Current InputState
		InputState m_state{};

		/// Previous InputState (last frame)
		InputState m_prev_state{};

		/// Tracks all VirtualInput to be updated automatically each frame,
		/// stores a non-owning raw pointer
		std::vector<VirtualInput*> m_virtual_inputs;
	};
} // namespace Ember
