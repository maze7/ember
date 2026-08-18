#pragma once

#include <ember/input/input.h>
#include <ember/math/math.h>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <variant>

namespace ember
{
	/**
	 * Bit flags used to include/exclude binding entries at runtime.
	 *
	 * The game defines the meaning of each bit (e.g. gameplay vs menu bindings).
	 * An entry with masks == 0 is always active; otherwise it is active only while
	 * it shares at least one bit with the filter word (see VirtualInputs::binding_filters).
	 */
	using BindingMask = u32;

	/// Whether an entry with the given masks passes the active filters.
	[[nodiscard]] constexpr bool binding_included(BindingMask masks, BindingMask filters) noexcept
	{
		return masks == 0 || (masks & filters) != 0;
	}

	/// A single frame of state sampled from a Binding.
	struct BindingState
	{
		/// Value became > 0 this frame.
		bool pressed = false;

		/// Value became 0 this frame.
		bool released = false;

		/// Value is currently > 0.
		bool down = false;

		/// Current state: 0 = unpressed, 1 = fully pressed.
		f32 value = 0.0f;

		/// When the underlying input last changed.
		u64 timestamp_ns = 0;
	};

	/// How an axis resolves its negative and positive bindings when both are held.
	enum class BindingAxisOverlap : u8
	{
		/// Use whichever input was pressed most recently.
		TakeNewer,

		/// Use whichever input was pressd longest ago.
		TakeOlder,

		/// Inputs cancel each other out.
		CancelOut,
	};

	/// Combines a negative/positive binding pair into a single [-1, 1] value.
	[[nodiscard]] constexpr f32 resolve_axis_overlap(
		BindingAxisOverlap overlap, const BindingState& negative, const BindingState& positive) noexcept
	{
		using enum BindingAxisOverlap;

		switch (overlap)
		{
			case CancelOut:
				return std::clamp(positive.value - negative.value, -1.0f, 1.0f);

			case TakeNewer:
				if (positive.down && negative.down)
					return negative.timestamp_ns > positive.timestamp_ns ? -negative.value : positive.value;
				if (positive.down)
					return positive.value;
				if (negative.down)
					return -negative.value;
				return 0.0f;

			case TakeOlder:
				if (positive.down && negative.down)
					return negative.timestamp_ns < positive.timestamp_ns ? -negative.value : positive.value;
				if (positive.down)
					return positive.value;
				if (negative.down)
					return -negative.value;
				return 0.0f;
		}

		return 0.0f;
	}

	/// Binding mapped to a keyboard Key. Device independent.
	struct KeyBinding
	{
		Key key = Key::Unknown;

		[[nodiscard]] BindingState state(const Input& input, u32 /* device */) const noexcept
		{
			const Keyboard& keyboard = input.keyboard();
			const bool down			 = keyboard.down(key);

			return {
				.pressed	  = keyboard.pressed(key),
				.released	  = keyboard.released(key),
				.down		  = down,
				.value		  = down ? 1.0f : 0.0f,
				.timestamp_ns = keyboard.timestamp(key),
			};
		}
	};

	/// Binding mapped to a MouseButton. Device independent.
	struct MouseButtonBinding
	{
		MouseButton button = MouseButton::None;

		[[nodiscard]] BindingState state(const Input& input, u32 /* device */) const noexcept
		{
			const Mouse& mouse = input.mouse();
			const bool down	   = mouse.down(button);

			return {
				.pressed	  = mouse.pressed(button),
				.released	  = mouse.released(button),
				.down		  = down,
				.value		  = down ? 1.0f : 0.0f,
				.timestamp_ns = mouse.button_timestamp(button),
			};
		}
	};

	/**
	 * Binding mapped to mouse motion along a directoin. The frame's motion is
	 * projected onto `axis` then remapped so `sign * min` pixels of travel is
	 * 0 and `sign * max` pixels is 1. Device independent.
	 */
	struct MouseMotionBinding
	{
		/// The direction of mouse motion to track (window space, +Y down).
		glm::vec2 axis{1.0f, 0.0f};

		/// The sign of the motion to track (+1 or -1).
		i32 sign = 1;

		/// Minimum travel (pixels) before the motion registers.
		f32 min = 0.0f;

		/// Travel (pixels) at which the value saturates to 1.
		f32 max = 25.0f;

		[[nodiscard]] BindingState state(const Input& input, u32 /* device */) const noexcept
		{
			const f32 value	 = sample(input.state());
			const f32 before = sample(input.last_state());

			return {
				.pressed	  = value > 0.0f && before <= 0.0f,
				.released	  = value <= 0.0f && before > 0.0f,
				.down		  = value > 0.0f,
				.value		  = value,
				.timestamp_ns = input.mouse().motion_timestamp(),
			};
		}

	private:
		[[nodiscard]] f32 sample(const InputState& state) const noexcept
		{
			const f32 travel = glm::dot(axis, state.mouse().delta());
			return clamped_map(travel, static_cast<f32>(sign) * min, static_cast<f32>(sign) * max);
		}
	};

	/// Binding mapped to a GamepadButton on the bound device slot.
	struct GamepadButtonBinding
	{
		GamepadButton button = GamepadButton::A;

		[[nodiscard]] BindingState state(const Input& input, u32 device) const noexcept
		{
			EMBER_ASSERT(device < Input::MAX_GAMEPADS);
			const Gamepad& gamepad = input.state().gamepads()[device];
			const bool down		   = gamepad.down(button);

			return {
				.pressed	  = gamepad.pressed(button),
				.released	  = gamepad.released(button),
				.down		  = down,
				.value		  = down ? 1.0f : 0.0f,
				.timestamp_ns = gamepad.timestamp(button),
			};
		}
	};

	/**
	 * Binding mapped to one direction of a GamepadAxis on the bound device slot.
	 * `sign` selects the direction, and the [deadzone, 1] range is remapped so
	 * the value spans the full [0, 1].
	 */
	struct GamepadAxisBinding
	{
		GamepadAxis axis = GamepadAxis::LeftX;

		/// The sign of the axis to track (+1 or -1).
		i32 sign = 1;

		/// Axis magnitude below which the binding reads 0.
		f32 deadzone = 0.0f;

		[[nodiscard]] BindingState state(const Input& input, u32 device) const noexcept
		{
			EMBER_ASSERT(device < Input::MAX_GAMEPADS);
			const Gamepad& gamepad = input.state().gamepads()[device];
			const f32 value		   = sample(input.state(), device);
			const f32 before	   = sample(input.last_state(), device);

			return {
				.pressed	  = value > 0.0f && before <= 0.0f,
				.released	  = value <= 0.0f && before > 0.0f,
				.down		  = value > 0.0f,
				.value		  = value,
				.timestamp_ns = gamepad.timestamp(axis),
			};
		}

	private:
		[[nodiscard]] f32 sample(const InputState& state, u32 device) const noexcept
		{
			EMBER_ASSERT(device < Input::MAX_GAMEPADS);
			const Gamepad& gamepad = state.gamepads()[device];
			const f32 value		   = gamepad.axis(axis);

			return clamped_map(value, static_cast<f32>(sign) * deadzone, static_cast<f32>(sign));
		}
	};

	/**
	 * A single hardware input mapped to a virtual control.
	 *
	 * Modeled as a closed variant: bindings are small values that can be copied,
	 * stored, and serialized freely. Rebinding UI can std::visit / std::get_if the source.
	 */
	struct Binding
	{
		using Source =
			std::variant<KeyBinding, MouseButtonBinding, MouseMotionBinding, GamepadButtonBinding, GamepadAxisBinding>;

		Source source;

		/// Samples the current state of this binding from the given Input.
		[[nodiscard]] BindingState state(const Input& input, u32 device) const noexcept
		{
			return std::visit([&](const auto& binding) { return binding.state(input, device); }, source);
		}
	};
}
