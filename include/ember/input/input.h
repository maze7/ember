#pragma once

#include <ember/input/keyboard.h>
#include <ember/input/mouse.h>
#include <ember/input/controller.h>

namespace ember
{
	/// Owns current and previous frame device state; a pure fold over the platform event stream.
	class Input
	{
	public:
		/// 4 controllers should be sufficient, right?
		static constexpr u8 MAX_CONTROLLERS = 4;

		/// A snapshot of all connected input devices for a single frame.
		struct FrameState
		{
			Controller controllers[MAX_CONTROLLERS];
			Keyboard keyboard;
			Mouse mouse;
		};

		/// Device state for the current frame.
		[[nodiscard]] FrameState& state() { return m_state; }
		[[nodiscard]] const FrameState& state() const { return m_state; }

		/// Device state from the previous frame.
		[[nodiscard]] FrameState& prev_state() { return m_prev_state; }
		[[nodiscard]] const FrameState& prev_state() const { return m_prev_state; }

		/// Mouse state for the current frame
		Mouse& mouse() { return m_state.mouse; }
		const Mouse& mouse() const { return m_state.mouse; }

		/// Mouse state for the previous frame
		Mouse& prev_mouse() { return m_prev_state.mouse; }
		const Mouse& prev_mouse() const { return m_prev_state.mouse; }

		/// Keyboard state for the current frame
		Keyboard& keyboard() { return m_state.keyboard; }
		const Keyboard& keyboard() const { return m_state.keyboard; }

		/// Keyboard state for the previous frame
		Keyboard& prev_keyboard() { return m_prev_state.keyboard; }
		const Keyboard& prev_keyboard() const { return m_prev_state.keyboard; }

		/// Controller state for the current frame.
		/// @param index Slot of the required controller.
		Controller& controller(u32 index)
		{
			EMBER_ASSERT(index < MAX_CONTROLLERS);
			return m_state.controllers[index];
		}

		/// Controller state for the current frame.
		/// @param index Slot of the required controller.
		const Controller& controller(u32 index) const
		{
			EMBER_ASSERT(index < MAX_CONTROLLERS);
			return m_state.controllers[index];
		}

		/// Controller state for the previous frame.
		/// @param index Slot of the required controller.
		Controller& prev_controller(u32 index)
		{
			EMBER_ASSERT(index < MAX_CONTROLLERS);
			return m_prev_state.controllers[index];
		}

		/// Controller state for the previous frame.
		/// @param index Slot of the required controller.
		const Controller& prev_controller(u32 index) const
		{
			EMBER_ASSERT(index < MAX_CONTROLLERS);
			return m_prev_state.controllers[index];
		}

	private:
		friend class Platform;

		/// Cycles state forward: previous <- current, then clears per-frame flags.
		void step_state();

		FrameState m_state;
		FrameState m_prev_state;
	};
}
