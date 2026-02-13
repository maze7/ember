#pragma once

#include "axes.h"
#include "buttons.h"
#include "core/common.h"
#include "keys.h"
#include <vec2.hpp>

namespace Ember
{
	struct InputState;
	class Input;

	struct BindingState
	{
		bool pressed = false;
		bool released = false;
		bool down = false;
		float value = 0;
		float value_no_deadzone = 0;
		u64 timestamp = 0;
	};

	struct Binding
	{
		virtual ~Binding() = default;
		virtual BindingState get_state(Input& input, int device) = 0;
	};

	struct MouseButtonBinding final : Binding
	{
		explicit MouseButtonBinding(MouseButton button) : button(button) {}

		/// The MouseButton to be watched
		MouseButton button;

		/// Retrieve binding state from an instance of the Input class
		BindingState get_state(Input& input, int device) override;
	};

	struct MouseMotionBinding final : Binding
	{
		MouseMotionBinding(glm::vec2 axis, int sign, float min, float max)
		    : axis(axis), sign(sign), min(min), max(max) {}

		/// The Axis of Mouse Motion to track
		glm::vec2 axis;

		/// The Sign of the Mouse Motion to track
		int sign;

		/// The minimum distance before the mouse motion is tracked
		float min;

		/// The maximum distance before the mouse motion is tracked
		float max;

		BindingState get_state(Input& input, int device) override;

	private:
		float get_value(InputState& state);
	};

	struct KeyboardKeyBinding final : Binding
	{
		explicit KeyboardKeyBinding(Key key) : key(key) {}

		/// The Keyboard Key to be watched
		Key key;

		/// Retrieve binding state from an instance of the Input class
		BindingState get_state(Input& input, int device) override;
	};

	struct ControllerButtonBinding final : Binding
	{
		explicit ControllerButtonBinding(ControllerButton button) : button(button) {}

		/// The ControllerButton to be watched
		ControllerButton button;

		/// Retrieve binding state from an instance of the Input class
		BindingState get_state(Input& input, int device) override;
	};

	struct ControllerAxisBinding final : Binding
	{
		explicit ControllerAxisBinding(Axis axis, int sign, float deadzone)
		    : axis(axis), sign(sign), deadzone(deadzone) {}

		/// The Axis on the controller to be track
		Axis axis;

		/// THe sign of the axis to track
		int sign;

		/// The deadzone (no-input zone) from 0 on the given axis
		float deadzone;

		/// Retrieve binding state from an instance of the Input class
		BindingState get_state(Input& input, int device) override;

	private:
		float get_value(InputState& state, int device, float deadzone);
	};
}
