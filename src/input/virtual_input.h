#pragma once

#include "bindings.h"
#include "input.h"
#include "math/math.h"

namespace Ember
{
	class VirtualInput
	{
	public:
		explicit VirtualInput(Input& input, int controller = 0, float buffer = 0);

		~VirtualInput();

		/// @returns True if one of the VirtualInput Bindings was pressed this frame
		[[nodiscard]] bool pressed() const {
			return m_state.pressed;
		}

		/// @returns True if one of the VirtualInput Bindings was released this frame
		[[nodiscard]] bool released() const {
			return m_state.released;
		}

		/// @returns True if one of the VirtualInput Bindings was held down this frame
		[[nodiscard]] bool down() const {
			return m_state.down;
		}

		/// @returns Maximum value of all VirtualInput Bindings for this frame
		[[nodiscard]] float value() const {
			return m_state.value;
		}

		/// @returns Maximum value (with deadzone) of all VirtualInput Bindings for this frame
		[[nodiscard]] float value_no_deadzone() const {
			return m_state.value_no_deadzone;
		}

		/// @returns Timestamp of most recent Binding change
		[[nodiscard]] bool timestamp() const {
			return m_state.timestamp;
		}

		/// @returns True if the press event was consumed for the current frame
		[[nodiscard]] bool press_consumed() const {
			return m_press_consumed;
		}

		/**
		 * Adds a Binding to the internal bindings of the VirtualInput
		 * @tparam T Binding subclass
		 * @tparam Args constructor arguments
		 * @param args constructor arguments
		 * @returns Reference to this, for function chaining
		 */
		template <class T, class... Args>
		VirtualInput& add(Args&&... args) {
			m_bindings.emplace_back(make_ref<T>(std::forward<Args>(args)...));
			return *this;
		}

		/**
		 * Adds keys to the VirtualInput
		 * @param keys Initializer list of Keys to be added
		 * @returns Reference to this, for function chaining
		 */
		VirtualInput& add(std::initializer_list<Key> keys) {
			for(auto key : keys)
				add<KeyboardKeyBinding>(key);
			return *this;
		}

		/**
		 * Adds MouseButtons to the VirtualInput
		 * @param buttons Initializer list of MouseButtons to be added
		 */
		VirtualInput add(std::initializer_list<MouseButton> buttons) {
			for(auto button : buttons)
				add<MouseButtonBinding>(button);
			return *this;
		}

		/**
		 * Adds ControllerButtons to the VirtualInput
		 * @param buttons Initializer list of ControllerButtons to be added
		 */
		VirtualInput& add(std::initializer_list<ControllerButton> buttons) {
			for(auto button : buttons)
				add<ControllerButtonBinding>(button);
			return *this;
		}

		/**
		 * Shortcut method to add a Controller Axis binding.
		 * @param axis Axis to be treated as a button
		 * @param sign the sign (direction) that should be considered a press
		 * @param deadzone The deadzone (ignored section) of the Axis
		 * @return
		 */
		VirtualInput& add(Axis axis, int sign, float deadzone = 0) {
			return add<ControllerAxisBinding>(axis, sign, deadzone);
		}

		/// @returns BindingState for the current frame
		BindingState state() const {
			return m_state;
		}

		/// Updates the internal BindingState of the VirtualInput for the current frame.
		/// @param dt Delta time in seconds for buffer decay
		void update(float dt = 0.0f) {
			m_state = BindingState{};

			for(auto& binding : m_bindings) {
				auto state = binding->get_state(m_input, m_device);

				m_state.pressed |= state.pressed;
				m_state.released |= state.released;
				m_state.down |= state.down;
				m_state.value = max(m_state.value, state.value);
				m_state.timestamp = m_state.timestamp > state.timestamp ? m_state.timestamp : state.timestamp;
			}

			// Buffer logic: when pressed, fill the buffer timer
			if (m_state.pressed) {
				m_buffer_timer = m_buffer;
			}

			// Decay buffer timer
			if (m_buffer_timer > 0.0f) {
				m_buffer_timer -= dt;
			}

			if(pressed()) {
				m_press_consumed = true;
			}
		}

		/// @returns True if the input is buffered (pressed within buffer window), consumes the buffered press
		[[nodiscard]] bool buffered() {
			bool pressed = m_buffer_timer > 0.0f || m_state.pressed;

			if (pressed) {
				consume_buffer();
			}

			return pressed;
		}

		/// Consumes the buffer (call when the action is performed)
		void consume_buffer() {
			m_buffer_timer = 0.0f;
		}

	private:
		friend class Input;

		/// Reference to the Input class that manages this VirtualInput
		Input& m_input;

		/// Stores the BindingState for the current frame
		BindingState m_state{};

		float m_buffer = 0;
		float m_buffer_timer = 0;
		int m_device = 0;
		bool m_repeated = false;
		bool m_press_consumed = false;

		std::vector<Ref<Binding>> m_bindings;
	};
}
