#pragma once

#include <ember/input/gamepad.h>
#include <ember/input/keyboard.h>
#include <ember/input/mouse.h>

#include <optional>

namespace ember
{
	class Platform;

	class InputState final
	{
	public:
		static constexpr u32 MAX_GAMEPADS = 4;

		InputState()
		{
			for (u32 i = 0; i < MAX_GAMEPADS; ++i)
				m_gamepads[i].m_index = i;
		}

		[[nodiscard]] const Keyboard& keyboard() const noexcept { return m_keyboard; }

		[[nodiscard]] const Mouse& mouse() const noexcept { return m_mouse; }

		[[nodiscard]] std::span<const Gamepad> gamepads() const noexcept { return m_gamepads; }

		[[nodiscard]] const Gamepad* gamepad(GamepadId id) const noexcept
		{
			if (!id.is_valid())
				return nullptr;

			for (const Gamepad& gamepad : m_gamepads)
			{
				if (gamepad.id() == id && gamepad.connected())
					return &gamepad;
			}

			return nullptr;
		}

	private:
		friend class Input;

		void set_frame_time(u64 now_ns) noexcept
		{
			m_keyboard.set_frame_time(now_ns);
			m_mouse.set_frame_time(now_ns);

			for (Gamepad& gamepad : m_gamepads)
				gamepad.set_frame_time(now_ns);
		}

		void next_frame(u64 now_ns) noexcept
		{
			m_keyboard.next_frame(now_ns);
			m_mouse.next_frame(now_ns);

			for (Gamepad& gamepad : m_gamepads)
				gamepad.next_frame(now_ns);
		}

		void clear() noexcept
		{
			m_keyboard.clear();
			m_mouse.clear();

			for (Gamepad& gamepad : m_gamepads)
				gamepad.clear();
		}

		Keyboard m_keyboard;
		Mouse m_mouse;
		std::array<Gamepad, MAX_GAMEPADS> m_gamepads{};
	};

	/// Owns current and previous frame device state; a pure fold over the platform event stream.
	class Input
	{
	public:
		/// 4 controllers should be sufficient, right?
		static constexpr u8 MAX_GAMEPADS = 4;

		Input(const Input&)			   = delete;
		Input& operator=(const Input&) = delete;
		Input(Input&&)				   = delete;
		Input& operator=(Input&&)	   = delete;

		[[nodiscard]] const InputState& state() const noexcept { return m_state; }

		[[nodiscard]] const InputState& last_state() const noexcept { return m_last_state; }

		[[nodiscard]] const Keyboard& keyboard() const noexcept { return m_state.keyboard(); }

		[[nodiscard]] const Mouse& mouse() const noexcept { return m_state.mouse(); }

		[[nodiscard]] std::span<const Gamepad> gamepads() const noexcept { return m_state.gamepads(); }

		[[nodiscard]] const Gamepad* gamepad(GamepadId id) const noexcept { return m_state.gamepad(id); }

		void clear() noexcept
		{
			m_last_state.clear();
			m_state.clear();
			m_next_state.clear();
		}

		[[nodiscard]] bool gamepad_in_use() const noexcept
		{
			const u64 keyboard_or_mouse = std::max(keyboard().input_timestamp(), mouse().input_timestamp());

			for (const Gamepad& gamepad : gamepads())
			{
				if (gamepad.connected() && gamepad.input_timestamp() > keyboard_or_mouse)
					return true;
			}

			return false;
		}

	private:
		friend class Platform;

		void publish(u64 now_ns)
		{
			m_last_state = m_state;
			m_state		 = m_next_state;

			// State is the snapshot queried during this update.
			m_state.set_frame_time(now_ns);

			// next_state preserves levels but clears frame transients.
			m_next_state.next_frame(now_ns);
		}

		void on_key(Key key, bool down, bool repeat, u64 timestamp) noexcept
		{
			m_next_state.m_keyboard.on_key(key, down, repeat, timestamp);
		}

		void on_text(std::string_view text, WindowHandle window) noexcept
		{
			m_next_state.m_keyboard.on_text(text, window);
		}

		void on_composition(std::string_view text, i32 selection_start, i32 selection_length, WindowHandle window) noexcept
		{
			m_next_state.m_keyboard.on_composition(text, selection_start, selection_length, window);
		}

		void on_mouse_button(MouseButton button, bool down, WindowHandle window, u64 timestamp) noexcept
		{
			m_next_state.m_mouse.on_button(button, down, window, timestamp);
		}

		void on_mouse_move(glm::vec2 position, glm::vec2 delta, WindowHandle window, u64 timestamp) noexcept
		{
			m_next_state.m_mouse.on_move(position, delta, window, timestamp);
		}

		void on_mouse_wheel(glm::vec2 wheel, WindowHandle window, u64 timestamp) noexcept
		{
			m_next_state.m_mouse.on_wheel(wheel, window, timestamp);
		}

		[[nodiscard]] std::optional<u32> connect_gamepad(GamepadId id, const GamepadInfo& info, u64 timestamp) noexcept
		{
			// Duplicate add: refresh metadata, retain the same slot.
			for (Gamepad& gamepad : m_next_state.m_gamepads)
			{
				if (gamepad.connected() && gamepad.id() == id)
				{
					gamepad.remap(info);
					return gamepad.index();
				}
				else if (!gamepad.connected())
				{
					gamepad.connect(id, info, timestamp);
					return gamepad.index();
				}
			}

			return std::nullopt;
		}

		void remap_gamepad(GamepadId id, const GamepadInfo& info) noexcept
		{
			for (Gamepad& gamepad : m_next_state.m_gamepads)
			{
				if (gamepad.connected() && gamepad.id() == id)
				{
					gamepad.remap(info);
					return;
				}
			}
		}

		void disconnect_gamepad(GamepadId id, u64 timestamp) noexcept
		{
			for (Gamepad& gamepad : m_next_state.m_gamepads)
			{
				if (gamepad.connected() && gamepad.id() == id)
				{
					gamepad.disconnect(timestamp);
					return;
				}
			}
		}

		void on_gamepad_button(GamepadId id, GamepadButton button, bool down, u64 timestamp) noexcept
		{
			for (Gamepad& gamepad : m_next_state.m_gamepads)
			{
				if (gamepad.connected() && gamepad.id() == id)
				{
					gamepad.on_button(button, down, timestamp);
					return;
				}
			}
		}

		void on_gamepad_axis(GamepadId id, GamepadAxis axis, f32 value, u64 timestamp) noexcept
		{
			for (Gamepad& gamepad : m_next_state.m_gamepads)
			{
				if (gamepad.connected() && gamepad.id() == id)
				{
					gamepad.on_axis(axis, value, timestamp);
					return;
				}
			}
		}

		void on_focus_lost(WindowHandle window, u64 timestamp) noexcept
		{
			m_next_state.m_keyboard.on_focus_lost(window, timestamp);
			m_next_state.m_mouse.on_focus_lost(window, timestamp);
		}

		InputState m_state;
		InputState m_last_state;
		InputState m_next_state;
	};
}
