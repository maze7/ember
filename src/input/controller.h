#pragma once

#include "axes.h"
#include "buttons.h"
#include <string>

namespace Ember
{
	/// Represents a Gamepad or Joystick device
	class Controller
	{
	public:
		static constexpr u8 MAX_BUTTONS = 64;
		static constexpr u8 MAX_AXES = 64;

		/**
		 * @return controller instance_id, this does not change while controller
		 * is connected.
		 */
		[[nodiscard]] u32 id() const {
			return m_id;
		}

		/**
		 * @return controller index within controllers array, this does not
		 * change while controller is connected.
		 */
		[[nodiscard]] int index() const {
			return m_index;
		}

		/**
		 * @return controller name
		 */
		[[nodiscard]] const std::string& name() const {
			return m_name;
		}

		/**
		 * @return whether controller is connected
		 */
		[[nodiscard]] const bool is_connected() const {
			return m_connected;
		}

		/**
		 * Returns whether a given controller button is currently being pressed
		 * down.
		 *
		 * @param button Controller button that is being checked
		 * @return boolean state of given controller button
		 */
		[[nodiscard]] bool down(ControllerButton button) const;

		/**
		 * Returns whether a given controller button was pressed in the current
		 * frame.
		 *
		 * @param button Controller button that is being checked
		 * @return boolean state of the given controller button
		 */
		[[nodiscard]] bool pressed(ControllerButton button) const;

		/**
		 * Returns whether a given controller button was released in the current
		 * frame.
		 *
		 * @param button Controller button that is being checked
		 * @return boolean state of the given controller button
		 */
		[[nodiscard]] bool released(ControllerButton button) const;

		/**
		 * Returns the current value of a given axis as a float.
		 *
		 * @param axis Gamepad or Joystick axis that is being checked
		 * @return float value of the given axis.
		 */
		[[nodiscard]] float axis(Axis axis) const;

		/**
		 * Rumbles the controller for a given duration. This will cancel any
		 * previous rumble effects.
		 * @param intensity from 0.0 to 1.0
		 * @param duration how long, in seconds, for the rumble to last
		 */
		void rumble(float intensity, float duration);

		/**
		 * Rumbles the controller for a given duration. This will cancel any
		 * previous rumble effects.
		 * @param low_intensity From 0.0 to 1.0 intensity of the Low-intensity
		 * rumble
		 * @param high_intensity From 0.0 to 1.0 intensity of the High-intensity
		 * rumble
		 * @param duration how long, in seconds, for the rumble to last.
		 */
		void rumble(float low_intensity, float high_intensity, float duration);

	private:
		friend class Input;
		friend class Window;

		/**
		 * Called by the window when an underlying SDL3 controller is connected.
		 *
		 * @param id u32 ID of the controller
		 * @param name device name of the controller
		 * @param is_gamepad boolean declaring whether the controller is a
		 * gamepad or joystick
		 * @param button_count number of buttons the controller supports
		 * @param axis_count number of axis the controller supports
		 * @param vendor USB vendor ID of the controller
		 * @param product USB product ID of the controller
		 * @param version USB product version of the controller
		 */
		void connect(u32 id, const std::string& name, bool is_gamepad, u8 button_count, u8 axis_count, u16 vendor, u16 product, u16 version);

		/**
		 * Called by the `Window` when an underlying SDL3 controller is
		 * disconnected
		 */
		void disconnect();

		/**
		 * Called by `Input` each frame when the `InputState` is stepped.
		 */
		void reset();

		/**
		 * Called by the `Window` when button press or release event occurs
		 * @param button Button on the controller that was pressed/released
		 * @param down Whether the controller was pressed or released
		 */
		void on_button(ControllerButton button, bool down);

		/**
		 * Called by the `Window` when an axis has a new value.
		 *
		 * @param axis Axis that has a new value
		 * @param value new value of the Axis
		 */
		void on_axis(Axis axis, float value);

		/// ID of the underlying SDL3 controller or joystick
		u32 m_id = 0;

		/// controller index within the controllers array, this will not change
		/// while the controller is connected
		u32 m_index = 0;

		/// controller name, provided by hardware manufacturer
		std::string m_name;

		/// array holding pressed state of each button
		bool m_pressed[MAX_BUTTONS] = {false};

		/// array holding the down state of each button
		bool m_down[MAX_BUTTONS] = {false};

		/// array holding the released state of each button
		bool m_released[MAX_BUTTONS] = {false};

		/// array holding the value for each axis
		float m_axis[MAX_AXES] = {0.0f};

		/// the timestamp for each button press
		u64 m_timestamp[MAX_BUTTONS];

		/// the timestamp for each axis
		u64 m_axis_timestamp[MAX_AXES];

		/// whether the controller is currently connected
		bool m_connected = false;

		/// whether the controller is a standard gamepad
		bool m_is_gamepad = false;

		/// number of available buttons for this controller
		u8 m_button_count = 0;

		/// number of available axes for this controller
		u8 m_axis_count = 0;

		/// USB Hardware vendor ID
		u16 m_vendor_id = 0;

		/// USB Product ID
		u16 m_product_id = 0;

		/// USB Product version
		u16 m_product_version = 0;
	};
} // namespace Ember
