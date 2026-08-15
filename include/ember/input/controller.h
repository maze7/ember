#pragma once

#include <ember/core/common.h>

namespace ember
{
	enum class ControllerButton : i32
	{
		None = -1,
		A = 0,
		B = 1,
		X = 2,
		Y = 3,
		Back = 4,
		Select = 5,
		Start = 6,
		LeftStick = 7,
		RightStick = 8,
		LeftShoulder = 9,
		RightShoulder = 10,
		Up = 11,
		Down = 12,
		Left = 13,
		Right = 14,
	};

	enum class ControllerAxis : i32
	{
		None		 = -1,
		LeftX		 = 0,
		LeftY		 = 1,
		RightX		 = 2,
		RightY		 = 3,
		TriggerLeft	 = 4,
		TriggerRight = 5
	};

	class Controller
	{
	public:
		static constexpr u8 MAX_BUTTONS = 64;
		static constexpr u8 MAX_AXES = 64;

		/**
		 * @returns slot index within Input's controller array.
		 * @note stable while connected.
		 */
		[[nodiscard]] int index() const { return m_index; }

		/** @returns controller connectivity state */
		[[nodiscard]] bool is_connected() const { return m_connected; }

		/**
		 * Returns whether a given controller button is currently being pressed
		 * down.
		 *
		 * @param button Controller button that is being checked
		 * @return boolean state of given controller button
		 */
		[[nodiscard]] bool down(ControllerButton button) const { return m_down[index_of(button)]; }

		/**
		 * Returns whether a given controller button was pressed in the current
		 * frame.
		 *
		 * @param button Controller button that is being checked
		 * @return boolean state of the given controller button
		 */
		[[nodiscard]] bool pressed(ControllerButton button) const { return m_pressed[index_of(button)]; }

		/**
		 * Returns whether a given controller button was released in the current
		 * frame.
		 *
		 * @param button Controller button that is being checked
		 * @return boolean state of the given controller button
		 */
		[[nodiscard]] bool released(ControllerButton button) const { return m_released[index_of(button)]; }

		/**
		 * Returns a timestamp of when the last ControllerButton was pressed.
		 *
		 * @param button ControllerButton that is being checked
		 * @return u64 timestamp of the last time the ControllerButton was pressed.
		 */
		[[nodiscard]] u64 timestamp(ControllerButton button) const { return m_timestamp[index_of(button)]; }

		/**
		 * Returns a timestamp of the last Axis input for a given axis.
		 *
		 * @param axis The Axis that is being requested
		 * @param u64 timestamp of the last Axis input
		 */
		[[nodiscard]] u64 timestamp(ControllerAxis axis) const { return m_axis_timestamp[index_of(axis)]; }

		/**
		 * Returns the current value of a given axis as a float.
		 *
		 * @param axis Gamepad or Joystick axis that is being checked
		 * @return float value of the given axis.
		 */
		[[nodiscard]] float axis(ControllerAxis axis) const { return m_axis[index_of(axis)]; }

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

		/// @returns the index of the given ControlerButton
		static u32 index_of(ControllerButton button)
		{
			const auto i = static_cast<u32>(button);
			EMBER_ASSERT(i < MAX_BUTTONS);
			return i;
		}

		/// @returns the index of the given ControllerAxis
		static u32 index_of(ControllerAxis axis)
		{
			const auto i = static_cast<u32>(axis);
			EMBER_ASSERT(i < MAX_AXES);
			return i;
		}

		/// Called by Input when the controller is connected.
		void connect(u32 slot_index)
		{
			*this = Controller{};
			m_index = slot_index;
			m_connected = true;
		}

		/// Called by Input when the controller is disconnected.
		void disconnect()
		{
			*this = Controller{};
		}

		/// Called by Input when a button is pressed on this Controller.
		void on_button(ControllerButton button, bool down, u64 timestamp)
		{
			const u32 i = index_of(button);
			if (down)
			{
				m_down[i] = true;
				m_pressed[i] = true;
				m_timestamp[i] = timestamp;
			}
			else
			{
				m_down[i] = false;
				m_released[i] = true;
			}
		}

		/// Called by Input when an axis is
		void on_axis(ControllerAxis axis, f32 value, u64 timestamp)
		{
			const u32 i = index_of(axis);
			m_axis[i] = value;
			m_axis_timestamp[i] = timestamp;
		}

		/// Clears per-frame flags. Connection, held state, and axes persist.
		void reset()
		{
			std::memset(m_pressed, 0, sizeof(m_pressed));
			std::memset(m_released, 0, sizeof(m_released));
		}

		/// controller index within the controllers array, this will not change
		/// while the controller is connected
		u32 m_index = 0;

		/// array holding pressed state of each button
		bool m_pressed[MAX_BUTTONS] = {false};

		/// array holding the down state of each button
		bool m_down[MAX_BUTTONS] = {false};

		/// array holding the released state of each button
		bool m_released[MAX_BUTTONS] = {false};

		/// array holding the value for each axis
		float m_axis[MAX_AXES] = {0.0f};

		/// the timestamp for each button press
		u64 m_timestamp[MAX_BUTTONS] = {0};

		/// the timestamp for each axis
		u64 m_axis_timestamp[MAX_AXES] = {0};

		/// whether the controller is currently connected
		bool m_connected = false;
	};
};
