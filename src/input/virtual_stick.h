#pragma once

#include "input/virtual_axis.h"

namespace Ember
{
	class VirtualStick
	{
	public:
		/// constructors
		explicit VirtualStick(Input& input, VirtualAxis::Overlap overlap_behavior = VirtualAxis::Overlap::TakeNewer, int device = 0, float deadzone = 0);

		/// Circular deadzone to apply across the resulting 2D vector
		float deadzone = 0;

		/// @returns Reference to the X-Axis
		[[nodiscard]] VirtualAxis& x() {
			return m_xaxis;
		}

		/// @returns Reference to the Y-Axis
		[[nodiscard]] VirtualAxis& y() {
			return m_yaxis;
		}

		/// Updates the internal state of the VirtualStick's axes
		void update() {
			m_xaxis.negative().update();
			m_xaxis.positive().update();
			m_yaxis.negative().update();
			m_yaxis.positive().update();
		}

		/// @returns The current value of the VirtualStick on the x,y axes
		[[nodiscard]] glm::vec2 value() const;

		/// @returns The current value of the VirtualStick normalized.
		[[nodiscard]] glm::vec2 value_normalized() const;

		/**
		 * Adds a Keyboard Key mapping
		 * @param left Key for the left direction
		 * @param right Key for the right direction
		 * @param up Key for the up direction
		 * @param down Key for the down direction
		 * @return Reference to this for function chaining
		 */
		VirtualStick& add(Key left, Key right, Key up, Key down);

		/**
		 * Adds a ControllerButton mapping
		 * @param left The ControllerButton to be used for the left direction
		 * @param right The ControllerButton to be used for the right direction
		 * @param up The ControllerButton to be used for the up direction
		 * @param down The ControllerButton to be used for the down direction
		 * @return Reference to this for function chaining
		 */
		VirtualStick& add(ControllerButton left, ControllerButton right, ControllerButton up, ControllerButton down);

		/**
		 * Adds a Controller Axis mapping
		 * @param x Horizontal axis
		 * @param y Vertical axis
		 * @param x_deadzone Horizontal deadzone
		 * @param y_deadzone Vertical deadzone
		 * @return Reference to this for function chaining
		 */
		VirtualStick& add(Axis x, Axis y, float x_deadzone = 0, float y_deadzone = 0);

		/**
		 * Adds a Mouse motion mapping
		 * @param max_motion Maximum motion to be registered
		 * @return Reference to this for function chaining
		 */
		VirtualStick& add_mouse_motion(float max_motion = 25);

		/// Adds Keyboard arrow Keys as a binding
		/// @returns Reference to this for function chaining
		VirtualStick& add_arrow_keys();

		/// Adds Keyboard WASD Keys as a binding
		/// @returns Reference to this for function chaining
		VirtualStick& add_wasd();

		/// Adds a mapping for the left joystick on the Controller
		/// @returns Reference to this for function chaining
		VirtualStick& add_left_joystick(float x_deadzone = 0, float y_deadzone = 0);

		/// Adds a mapping for the right joystick on the Controller
		/// @returns Reference to this for function chaining
		VirtualStick& add_right_joystick(float x_deadzone = 0, float y_deadzone = 0);

		/// Adds a Controller DPAD mapping
		/// @returns Reference to this for function chaining
		VirtualStick& add_dpad();

	private:
		/// Reference to the Input that will update this VirtualStick
		Input& m_input;

		/// Horizontal VirtualAxis
		VirtualAxis m_xaxis;

		/// Vertical VirtualAxis
		VirtualAxis m_yaxis;
	};
}
