#pragma once

#include <ember/core/common.h>
#include <ember/input/common.h>
#include <ember/platform/window.h>

#include <array>
#include <bitset>
#include <optional>
#include <span>
#include <string_view>

namespace ember
{
	// These are generally copied from the SDL3 Scancode Keys,
	// which are in turn based on the USB standards:
	// https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf
	enum class Key : u32
	{
		Unknown			 = 0,
		A				 = 4,
		B				 = 5,
		C				 = 6,
		D				 = 7,
		E				 = 8,
		F				 = 9,
		G				 = 10,
		H				 = 11,
		I				 = 12,
		J				 = 13,
		K				 = 14,
		L				 = 15,
		M				 = 16,
		N				 = 17,
		O				 = 18,
		P				 = 19,
		Q				 = 20,
		R				 = 21,
		S				 = 22,
		T				 = 23,
		U				 = 24,
		V				 = 25,
		W				 = 26,
		X				 = 27,
		Y				 = 28,
		Z				 = 29,
		D1				 = 30,
		D2				 = 31,
		D3				 = 32,
		D4				 = 33,
		D5				 = 34,
		D6				 = 35,
		D7				 = 36,
		D8				 = 37,
		D9				 = 38,
		D0				 = 39,
		Enter			 = 40,
		Escape			 = 41,
		Backspace		 = 42,
		Tab				 = 43,
		Space			 = 44,
		Minus			 = 45,
		Equals			 = 46,
		LeftBracket		 = 47,
		RightBracket	 = 48,
		Backslash		 = 49,
		Semicolon		 = 51,
		Apostrophe		 = 52,
		Tilde			 = 53,
		Comma			 = 54,
		Period			 = 55,
		Slash			 = 56,
		Capslock		 = 57,
		F1				 = 58,
		F2				 = 59,
		F3				 = 60,
		F4				 = 61,
		F5				 = 62,
		F6				 = 63,
		F7				 = 64,
		F8				 = 65,
		F9				 = 66,
		F10				 = 67,
		F11				 = 68,
		F12				 = 69,
		F13				 = 104,
		F14				 = 105,
		F15				 = 106,
		F16				 = 107,
		F17				 = 108,
		F18				 = 109,
		F19				 = 110,
		F20				 = 111,
		F21				 = 112,
		F22				 = 113,
		F23				 = 114,
		F24				 = 115,
		PrintScreen		 = 70,
		ScrollLock		 = 71,
		Pause			 = 72,
		Insert			 = 73,
		Home			 = 74,
		PageUp			 = 75,
		Delete			 = 76,
		End				 = 77,
		PageDown		 = 78,
		Right			 = 79,
		Left			 = 80,
		Down			 = 81,
		Up				 = 82,
		Numlock			 = 83,
		Application		 = 101,
		Execute			 = 116,
		Help			 = 117,
		Menu			 = 118,
		Select			 = 119,
		Stop			 = 120,
		Redo			 = 121,
		Undo			 = 122,
		Cut				 = 123,
		Copy			 = 124,
		Paste			 = 125,
		Find			 = 126,
		Mute			 = 127,
		VolumeUp		 = 128,
		VolumeDown		 = 129,
		AltErase		 = 153,
		SysReq			 = 154,
		Cancel			 = 155,
		Clear			 = 156,
		Prior			 = 157,
		Enter2			 = 158,
		Separator		 = 159,
		Out				 = 160,
		Oper			 = 161,
		ClearAgain		 = 162,
		KeypadA			 = 188,
		KeypadB			 = 189,
		KeypadC			 = 190,
		KeypadD			 = 191,
		KeypadE			 = 192,
		KeypadF			 = 193,
		Keypad0			 = 98,
		Keypad00		 = 176,
		Keypad000		 = 177,
		Keypad1			 = 89,
		Keypad2			 = 90,
		Keypad3			 = 91,
		Keypad4			 = 92,
		Keypad5			 = 93,
		Keypad6			 = 94,
		Keypad7			 = 95,
		Keypad8			 = 96,
		Keypad9			 = 97,
		KeypadDivide	 = 84,
		KeypadMultiply	 = 85,
		KeypadMinus		 = 86,
		KeypadPlus		 = 87,
		KeypadEnter		 = 88,
		KeypadPeriod	 = 99,
		KeypadEquals	 = 103,
		KeypadComma		 = 133,
		KeypadLeftParen	 = 182,
		KeypadRightParen = 183,
		KeypadLeftBrace	 = 184,
		KeypadRightBrace = 185,
		KeypadTab		 = 186,
		KeypadBackspace	 = 187,
		KeypadXor		 = 194,
		KeypadPower		 = 195,
		KeypadPercent	 = 196,
		KeypadLess		 = 197,
		KeypadGreater	 = 198,
		KeypadAmpersand	 = 199,
		KeypadColon		 = 203,
		KeypadHash		 = 204,
		KeypadSpace		 = 205,
		KeypadClear		 = 216,
		LeftControl		 = 224,
		LeftShift		 = 225,
		LeftAlt			 = 226,
		LeftOS			 = 227,
		RightControl	 = 228,
		RightShift		 = 229,
		RightAlt		 = 230,
		RightOS			 = 231,

		Count = 232,
	};

	/**
	 * Per-frame keyboard state, keyed by physical scancode (layout-independent).
	 *
	 * Semantics: down() is level (held right now), pressed()/released() are edges
	 * (happened this frame, cleared by the next update). OS key repeats do not
	 * re-trigger pressed(). Fed by Input::update(); game code receives it read only.
	 */
	class Keyboard final
	{
	public:
		static constexpr u32 KEY_COUNT = static_cast<u32>(Key::Count);

		struct Composition
		{
			std::string_view text;
			i32 selection_start	 = -1;
			i32 selection_length = 0;
			bool active			 = false;
		};

		Keyboard()
		{
			m_text.reserve(64);
			m_composition.reserve(64);
		}

		Keyboard(const Keyboard& other) : Keyboard() { *this = other; }

		Keyboard& operator=(const Keyboard& other)
		{
			if (this == &other)
				return *this;

			m_down	   = other.m_down;
			m_pressed  = other.m_pressed;
			m_released = other.m_released;

			m_timestamps = other.m_timestamps;

			// Copy contents while retaining this String's PMR allocator.
			m_text.assign(other.m_text.data(), other.m_text.size());
			m_composition.assign(other.m_composition.data(), other.m_composition.size());

			m_composition_selection_start  = other.m_composition_selection_start;
			m_composition_selection_length = other.m_composition_selection_length;
			m_focused_window			   = other.m_focused_window;
			m_input_timestamp			   = other.m_input_timestamp;
			m_previous_frame_ns			   = other.m_previous_frame_ns;
			m_frame_ns					   = other.m_frame_ns;

			return *this;
		}

		[[nodiscard]] bool down(Key key) const noexcept { return m_down[index(key)]; }

		[[nodiscard]] bool pressed(Key key) const noexcept { return m_pressed[index(key)]; }

		[[nodiscard]] bool released(Key key) const noexcept { return m_released[index(key)]; }

		[[nodiscard]] bool repeated(Key key, RepeatConfig config = {}) const noexcept
		{
			u32 i = index(key);

			return input_detail::repeated(
				m_pressed[i], m_down[i], m_timestamps[i], m_previous_frame_ns, m_frame_ns, config);
		}

		[[nodiscard]] bool pressed_or_repeated(Key key, RepeatConfig config = {}) const noexcept
		{
			return repeated(key, config);
		}

		[[nodiscard]] bool down(std::span<const Key> keys) const noexcept
		{
			for (const Key key : keys)
			{
				if (down(key))
					return true;
			}

			return false;
		}

		[[nodiscard]] bool pressed(std::span<const Key> keys) const noexcept
		{
			for (const Key key : keys)
			{
				if (pressed(key))
					return true;
			}

			return false;
		}

		[[nodiscard]] bool released(std::span<const Key> keys) const noexcept
		{
			for (const Key key : keys)
			{
				if (released(key))
					return true;
			}

			return false;
		}

		[[nodiscard]] std::optional<Key> first_down() const noexcept
		{
			for (u32 i = 1; i < KEY_COUNT; ++i)
			{
				if (m_down[i])
					return static_cast<Key>(i);
			}

			return std::nullopt;
		}

		[[nodiscard]] std::optional<Key> first_pressed() const noexcept
		{
			for (u32 i = 1; i < KEY_COUNT; ++i)
			{
				if (m_pressed[i])
					return static_cast<Key>(i);
			}

			return std::nullopt;
		}

		[[nodiscard]] u64 timestamp(Key key) const noexcept { return m_timestamps[index(key)]; }

		[[nodiscard]] u64 input_timestamp() const noexcept { return m_input_timestamp; }

		[[nodiscard]] std::string_view text() const noexcept { return m_text; }

		[[nodiscard]] Composition composition() const noexcept
		{
			return {
				.text			  = m_composition,
				.selection_start  = m_composition_selection_start,
				.selection_length = m_composition_selection_length,
				.active			  = !m_composition.empty(),
			};
		}

		[[nodiscard]] WindowHandle focused_window() const noexcept { return m_focused_window; }

		[[nodiscard]] bool control() const noexcept { return down(Key::LeftControl) || down(Key::RightControl); }

		[[nodiscard]] bool shift() const noexcept { return down(Key::LeftShift) || down(Key::RightShift); }

		[[nodiscard]] bool alt() const noexcept { return down(Key::LeftAlt) || down(Key::RightAlt); }

	private:
		friend class Input;
		friend class InputState;

		using KeyBits = std::bitset<KEY_COUNT>;

		[[nodiscard]] static u32 index(Key key) noexcept
		{
			u32 value = static_cast<u32>(key);
			EMBER_ASSERT(value < KEY_COUNT);
			return value;
		}

		void set_frame_time(u64 now_ns) noexcept
		{
			m_previous_frame_ns = m_frame_ns;
			m_frame_ns			= now_ns;
		}

		void next_frame(u64 now_ns) noexcept
		{
			m_pressed.reset();
			m_released.reset();
			m_text.clear();
			set_frame_time(now_ns);
		}

		void clear() noexcept
		{
			m_down.reset();
			m_pressed.reset();
			m_released.reset();
			m_timestamps.fill(0);

			m_text.clear();
			m_composition.clear();

			m_composition_selection_start  = -1;
			m_composition_selection_length = 0;

			m_focused_window	= {};
			m_input_timestamp	= 0;
			m_previous_frame_ns = 0;
			m_frame_ns			= 0;
		}

		void on_key(Key key, bool down, bool repeat, u64 timestamp) noexcept
		{
			if (key == Key::Unknown || repeat)
				return;

			u32 i		  = index(key);
			bool was_down = m_down[i];

			if (down == was_down)
				return;

			m_down[i] = down;

			if (down)
				m_pressed[i] = true;
			else
				m_released[i] = true;

			m_timestamps[i]	  = timestamp;
			m_input_timestamp = timestamp;
		}

		void on_text(std::string_view text, WindowHandle window) noexcept
		{
			m_text.append(text.data(), text.size());
			m_composition.clear();
			m_composition_selection_start  = -1;
			m_composition_selection_length = 0;
			m_focused_window			   = window;
		}

		void
		on_composition(std::string_view text, i32 selection_start, i32 selection_length, WindowHandle window) noexcept
		{
			m_composition.assign(text.data(), text.size());
			m_composition_selection_start  = selection_start;
			m_composition_selection_length = selection_length;
			m_focused_window			   = window;
		}

		void on_focus_lost(WindowHandle window, u64 timestamp) noexcept
		{
			if (!m_focused_window.is_null() && m_focused_window != window)
				return;

			m_released |= m_down;

			for (u32 i = 0; i < KEY_COUNT; ++i)
			{
				if (m_down[i])
					m_timestamps[i] = timestamp;
			}

			m_down.reset();
			m_text.clear();
			m_composition.clear();
			m_composition_selection_start  = -1;
			m_composition_selection_length = 0;
			m_focused_window			   = {};
			m_input_timestamp			   = timestamp;
		}

		KeyBits m_down{};
		KeyBits m_pressed{};
		KeyBits m_released{};

		std::array<u64, KEY_COUNT> m_timestamps{};

		String m_text;
		String m_composition;

		i32 m_composition_selection_start  = -1;
		i32 m_composition_selection_length = 0;

		WindowHandle m_focused_window{};

		u64 m_input_timestamp	= 0;
		u64 m_previous_frame_ns = 0;
		u64 m_frame_ns			= 0;
	};
}
