#pragma once

#include <ember/core/common.h>
#include <ember/core/handle.h>
#include <ember/input/common.h>
#include <glm/vec2.hpp>

#include <bitset>

namespace ember
{
	struct GamepadId
	{
		u32 value = 0;

		[[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }

		constexpr bool operator<=>(const GamepadId&) const noexcept = default;
	};

	enum class GamepadButton : u8
	{
		A,
		B,
		X,
		Y,
		Back,
		Guide,
		Start,
		LeftStick,
		RightStick,
		LeftShoulder,
		RightShoulder,
		Up,
		Down,
		Left,
		Right,
		Count,
	};

	enum class GamepadAxis : u8
	{
		LeftX,
		LeftY,
		RightX,
		RightY,
		LeftTrigger,
		RightTrigger,
		Count,
	};

	enum class GamepadType : u8
	{
		Unknown,
		Standard,
		Xbox360,
		XboxOne,
		PlayStation3,
		PlayStation4,
		PlayStation5,
		SwitchPro,
		SwitchJoyConLeft,
		SwitchJoyConRight,
		SwitchJoyConPair,
		GameCube,
	};

	struct GamepadInfo
	{
		static constexpr size_t NAME_CAPACITY = 128;

		std::array<char, NAME_CAPACITY> name_buffer{};
		GamepadType type = GamepadType::Unknown;

		u16 vendor			= 0;
		u16 product			= 0;
		u16 product_version = 0;

		[[nodiscard]] std::string_view name() const noexcept
		{
			size_t length = 0;
			while (length < name_buffer.size() && name_buffer[length] != '\0')
				++length;

			return {name_buffer.data(), length};
		}
	};

	class Gamepad final
	{
	public:
		static constexpr u32 BUTTON_COUNT = static_cast<u32>(GamepadButton::Count);
		static constexpr u32 AXIS_COUNT	  = static_cast<u32>(GamepadAxis::Count);

		[[nodiscard]] u32 index() const noexcept { return m_index; }

		[[nodiscard]] GamepadId id() const noexcept { return m_id; }

		[[nodiscard]] bool connected() const noexcept { return m_connected; }

		[[nodiscard]] const GamepadInfo& info() const noexcept { return m_info; }

		[[nodiscard]] bool down(GamepadButton button) const { return m_down[index(button)]; }

		[[nodiscard]] bool pressed(GamepadButton button) const { return m_pressed[index(button)]; }

		[[nodiscard]] bool released(GamepadButton button) const { return m_released[index(button)]; }

		[[nodiscard]] f32 axis(GamepadAxis axis) const { return m_axes[index(axis)]; }

		[[nodiscard]] bool axis_changed(GamepadAxis axis) const noexcept { return m_changed_axes[index(axis)]; }

		[[nodiscard]] u64 timestamp(GamepadButton button) const { return m_button_timestamps[index(button)]; }

		[[nodiscard]] u64 timestamp(GamepadAxis axis) const { return m_axis_timestamps[index(axis)]; }

		[[nodiscard]] u64 input_timestamp() const noexcept { return m_input_timestamp; }

		[[nodiscard]] bool repeated(GamepadButton button, RepeatConfig config = {}) const noexcept
		{
			u32 i = index(button);

			return input_detail::repeated(
				m_pressed[i], m_down[i], m_button_timestamps[i], m_previous_frame_ns, m_frame_ns, config);
		}

		[[nodiscard]] glm::vec2 left_stick() const noexcept
		{
			return {axis(GamepadAxis::LeftX), axis(GamepadAxis::LeftY)};
		}

		[[nodiscard]] glm::vec2 right_stick() const noexcept
		{
			return {axis(GamepadAxis::RightX), axis(GamepadAxis::RightY)};
		}

	private:
		friend class InputState;
		friend class Input;

		using ButtonBits = std::bitset<BUTTON_COUNT>;
		using AxisBits	 = std::bitset<AXIS_COUNT>;

		[[nodiscard]] static constexpr u32 index(GamepadButton button)
		{
			const u32 value = static_cast<u32>(button);
			EMBER_ASSERT(value < BUTTON_COUNT);
			return value;
		}

		[[nodiscard]] static constexpr u32 index(GamepadAxis axis)
		{
			const u32 value = static_cast<u32>(axis);
			EMBER_ASSERT(value < AXIS_COUNT);
			return value;
		}

		void set_frame_time(u64 now_ns) noexcept
		{
			m_previous_frame_ns = m_frame_ns;
			m_frame_ns			= now_ns;
		}

		void begin_frame(u64 now_ns) noexcept
		{
			m_pressed.reset();
			m_released.reset();
			m_changed_axes.reset();
			set_frame_time(now_ns);
		}

		void connect(GamepadId id, const GamepadInfo& info, u64 timestamp)
		{
			const u32 slot = m_index;

			*this	= Gamepad{};
			m_index = slot;

			m_id			  = id;
			m_info			  = info;
			m_connected		  = true;
			m_input_timestamp = timestamp;
		}

		void remap(const GamepadInfo& info) { m_info = info; }

		void disconnect(u64 timestamp)
		{
			m_released |= m_down;
			m_down.reset();

			for (u32 i = 0; i < AXIS_COUNT; ++i)
			{
				if (m_axes[i] != 0.0f)
					m_changed_axes[i] = true;
			}

			m_axes.fill(0.0f);
			m_connected		  = false;
			m_input_timestamp = timestamp;

			// Keep id/info for this disconnected snapshot. The next connection
			// assigned to this slot replaces them.
		}

		void clear() noexcept
		{
			m_down.reset();
			m_pressed.reset();
			m_released.reset();
			m_changed_axes.reset();

			m_axes.fill(0.0f);
			m_button_timestamps.fill(0);
			m_axis_timestamps.fill(0);
			m_input_timestamp = 0;
		}

		void on_button(GamepadButton button, bool down, u64 timestamp_ns)
		{
			if (!m_connected)
				return;

			const u32 i	   = index(button);
			const bool was_down = m_down[i];

			if (down == was_down)
				return;

			m_down[i] = down;

			if (down)
				m_pressed[i] = true;
			else
				m_released[i] = true;

			m_button_timestamps[i] = timestamp_ns;
			m_input_timestamp	   = timestamp_ns;
		}

		void on_axis(GamepadAxis axis, i16 value, u64 timestamp_ns)
		{
			if (!m_connected)
				return;

			const u32 i = index(axis);

			if (m_axes[i] == value)
				return;

			m_axes[i]			 = value;
			m_changed_axes[i]	 = true;
			m_axis_timestamps[i] = timestamp_ns;

			// Avoid analog noise taking over "last used device."
			if (std::abs(value) >= 0.5f)
				m_input_timestamp = timestamp_ns;
		}

		u32 m_index = 0;

		GamepadId m_id{};
		GamepadInfo m_info{};

		ButtonBits m_down{};
		ButtonBits m_pressed{};
		ButtonBits m_released{};
		AxisBits m_changed_axes{};

		std::array<f32, AXIS_COUNT> m_axes{};
		std::array<u64, BUTTON_COUNT> m_button_timestamps{};
		std::array<u64, AXIS_COUNT> m_axis_timestamps{};

		u64 m_input_timestamp	= 0;
		u64 m_previous_frame_ns = 0;
		u64 m_frame_ns			= 0;

		bool m_connected = false;
	};
};
