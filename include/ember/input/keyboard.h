#pragma once

#include <cstring>
#include <ember/core/common.h>
#include <string_view>

namespace ember
{
	// These are generally copied from the SDL3 Scancode Keys,
	// which are in turn based on the USB standards:
	// https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf
	enum class Key : u32
	{
		Unknown = 0,
		A = 4,
		B = 5,
		C = 6,
		D = 7,
		E = 8,
		F = 9,
		G = 10,
		H = 11,
		I = 12,
		J = 13,
		K = 14,
		L = 15,
		M = 16,
		N = 17,
		O = 18,
		P = 19,
		Q = 20,
		R = 21,
		S = 22,
		T = 23,
		U = 24,
		V = 25,
		W = 26,
		X = 27,
		Y = 28,
		Z = 29,
		D1 = 30,
		D2 = 31,
		D3 = 32,
		D4 = 33,
		D5 = 34,
		D6 = 35,
		D7 = 36,
		D8 = 37,
		D9 = 38,
		D0 = 39,
		Enter = 40,
		Escape = 41,
		Backspace = 42,
		Tab = 43,
		Space = 44,
		Minus = 45,
		Equals = 46,
		LeftBracket = 47,
		RightBracket = 48,
		Backslash = 49,
		Semicolon = 51,
		Apostrophe = 52,
		Tilde = 53,
		Comma = 54,
		Period = 55,
		Slash = 56,
		Capslock = 57,
		F1 = 58,
		F2 = 59,
		F3 = 60,
		F4 = 61,
		F5 = 62,
		F6 = 63,
		F7 = 64,
		F8 = 65,
		F9 = 66,
		F10 = 67,
		F11 = 68,
		F12 = 69,
		F13 = 104,
		F14 = 105,
		F15 = 106,
		F16 = 107,
		F17 = 108,
		F18 = 109,
		F19 = 110,
		F20 = 111,
		F21 = 112,
		F22 = 113,
		F23 = 114,
		F24 = 115,
		PrintScreen = 70,
		ScrollLock = 71,
		Pause = 72,
		Insert = 73,
		Home = 74,
		PageUp = 75,
		Delete = 76,
		End = 77,
		PageDown = 78,
		Right = 79,
		Left = 80,
		Down = 81,
		Up = 82,
		Numlock = 83,
		Application = 101,
		Execute = 116,
		Help = 117,
		Menu = 118,
		Select = 119,
		Stop = 120,
		Redo = 121,
		Undo = 122,
		Cut = 123,
		Copy = 124,
		Paste = 125,
		Find = 126,
		Mute = 127,
		VolumeUp = 128,
		VolumeDown = 129,
		AltErase = 153,
		SysReq = 154,
		Cancel = 155,
		Clear = 156,
		Prior = 157,
		Enter2 = 158,
		Separator = 159,
		Out = 160,
		Oper = 161,
		ClearAgain = 162,
		KeypadA = 188,
		KeypadB = 189,
		KeypadC = 190,
		KeypadD = 191,
		KeypadE = 192,
		KeypadF = 193,
		Keypad0 = 98,
		Keypad00 = 176,
		Keypad000 = 177,
		Keypad1 = 89,
		Keypad2 = 90,
		Keypad3 = 91,
		Keypad4 = 92,
		Keypad5 = 93,
		Keypad6 = 94,
		Keypad7 = 95,
		Keypad8 = 96,
		Keypad9 = 97,
		KeypadDivide = 84,
		KeypadMultiply = 85,
		KeypadMinus = 86,
		KeypadPlus = 87,
		KeypadEnter = 88,
		KeypadPeroid = 99,
		KeypadEquals = 103,
		KeypadComma = 133,
		KeypadLeftParen = 182,
		KeypadRightParen = 183,
		KeypadLeftBrace = 184,
		KeypadRightBrace = 185,
		KeypadTab = 186,
		KeypadBackspace = 187,
		KeypadXor = 194,
		KeypadPower = 195,
		KeypadPercent = 196,
		KeypadLess = 197,
		KeypadGreater = 198,
		KeypadAmpersand = 199,
		KeypadColon = 203,
		KeypadHash = 204,
		KeypadSpace = 205,
		KeypadClear = 216,
		LeftControl = 224,
		LeftShift = 225,
		LeftAlt = 226,
		LeftOS = 227,
		RightControl = 228,
		RightShift = 229,
		RightAlt = 230,
		RightOS = 231,

		Count = 232,
	};

	/**
	 * Per-frame keyboard state, keyed by physical scancode (layout-independent).
	 *
	 * Semantics: down() is level (held right now), pressed()/released() are edges
	 * (happened this frame, cleared by the next update). OS key repeats do not
	 * re-trigger pressed(). Fed by Input::update(); game code receives it read only.
	 */
	class Keyboard
	{
	public:
		static constexpr u32 MAX_KEYS = 512;

		/** @returns true if the given key was pressed during the current frame */
		[[nodiscard]] bool pressed(Key key) const { return m_pressed[index(key)]; }

		/** @returns true if the given key is currently held down */
		[[nodiscard]] bool down(Key key) const { return m_down[index(key)]; }

		/** @returns true if the given key was released during the current frame */
		[[nodiscard]] bool released(Key key) const { return m_released[index(key)]; }

		/** @returns the timestamp (ns) of the last press of the given key */
		[[nodiscard]] u64 timestamp(Key key) const { return m_timestamp[index(key)]; }

		/** @returns true if either of the CTRL keys are down */
		[[nodiscard]] bool ctrl() const { return down(Key::LeftControl) || down(Key::RightControl); }

		/** @returns true if either of the SHIFT keys are down */
		[[nodiscard]] bool shift() const { return down(Key::LeftShift) || down(Key::RightShift); }

		/** @returns true if either of the ALT keys are down */
		[[nodiscard]] bool alt() const { return down(Key::LeftAlt) || down(Key::RightAlt); }

		/** UTF-8 text entered this frame. */
		[[nodiscard]] std::string_view text() const { return {m_text, m_text_length}; }

	private:
		friend class Input;

		static u32 index(Key key)
		{
			const auto i = static_cast<u32>(key);
			EMBER_ASSERT(i < MAX_KEYS);
			return i;
		}

		/**
		 * Applies a key transition.
		 * @param timestamp OS event time in ns.
		 * @note fed by Input::update()
		 */
		void on_key(Key key, bool down, u64 timestamp)
		{
			const u32 i = index(key);
			if (down)
			{
				m_down[i]	   = true;
				m_pressed[i]   = true;
				m_timestamp[i] = timestamp;
			}
			else
			{
				m_down[i]	  = false;
				m_released[i] = true;
			}
		}

		/**
		 * Appends a UTF-8 chunk to this frame's text buffer; excess is dropped.
		 * @param utf8 view over the text buffer
		 * @note fed by Input::update()
		 */
		void on_text(std::string_view utf8)
		{
			for (u32 i = 0; i < utf8.size() && m_text_length < sizeof(m_text); ++i)
				m_text[m_text_length++] = utf8[i];
		}

		/** Clears per-frame edges and text; held state and timestamps persist. */
		void reset()
		{
			memset(m_pressed, 0, sizeof(m_pressed));
			memset(m_released, 0, sizeof(m_released));
			m_text_length = 0;
		}

		bool m_pressed[MAX_KEYS]{};
		bool m_down[MAX_KEYS]{};
		bool m_released[MAX_KEYS]{};
		u64 m_timestamp[MAX_KEYS]{};
		char m_text[32]{};
		u32 m_text_length = 0;
	};
}
