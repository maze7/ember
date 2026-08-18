#pragma once

#include <ember/input/binding_set.h>

#include <glm/vec2.hpp>

#include <memory>
#include <span>

namespace ember
{
	class VirtualInputs;
	class VirtualDevice;

	/// Frame timestamps driving virtual input updates, in the same nanosecond
	/// clock the platform stamps input events with.
	struct VirtualTime
	{
		u64 now_ns		= 0;
		u64 previous_ns = 0;
	};

	/**
	 * Base class for virtual inputs (actions, axes, sticks, devices).
	 *
	 * Instances register with a VirtualInputs system on construction and
	 * unregister on destruction (RAII replaces Foster's Dispose + weak refs).
	 * They are pinned in memory - non-copyable and non-movable - because the
	 * system holds pointers to them.
	 */
	class VirtualInput
	{
	public:
		VirtualInput(const VirtualInput&)			 = delete;
		VirtualInput& operator=(const VirtualInput&) = delete;
		VirtualInput(VirtualInput&&)				 = delete;
		VirtualInput& operator=(VirtualInput&&)		 = delete;

		virtual ~VirtualInput();

		[[nodiscard]] StringView name() const noexcept { return m_name; }

		/// Which gamepad slot this input reads (see Input::gamepads()).
		[[nodiscard]] u32 controller_index() const noexcept { return m_controller_index; }

		virtual void set_controller_index(u32 index) noexcept
		{
			EMBER_ASSERT(index < InputState::MAX_GAMEPADS);
			m_controller_index = index;
		}

		/// update() is only called while this is true.
		bool active = true;

	protected:
		VirtualInput(VirtualInputs& owner, StringView name, u32 controller_index);

		[[nodiscard]] VirtualInputs& owner() const noexcept { return *m_owner; }

	private:
		friend class VirtualInputs;

		/// Folds the freshly published Input state into this virtual input.
		/// Called once per frame by VirtualInputs::update.
		virtual void update(const Input& input, VirtualTime time, BindingMask filters) noexcept = 0;

		VirtualInputs* m_owner = nullptr;
		String m_name;
		u32 m_controller_index = 0;
	};

	/**
	 * Owns the per-frame update of every VirtualInput plus the active binding
	 * filters. Foster folds this into Input itself; it lives one layer above
	 * here so Input stays a pure fold over the platform event stream.
	 *
	 * Call update() exactly once per frame, right after Platform::pump_events,
	 * with the platform's current time so virtual timestamps stay comparable
	 * with input event timestamps.
	 */
	class VirtualInputs final
	{
	public:
		VirtualInputs() = default;

		VirtualInputs(const VirtualInputs&)			   = delete;
		VirtualInputs& operator=(const VirtualInputs&) = delete;
		VirtualInputs(VirtualInputs&&)				   = delete;
		VirtualInputs& operator=(VirtualInputs&&)	   = delete;

		~VirtualInputs()
		{
			// Every virtual input must be destroyed before its owning system.
			EMBER_ASSERT(m_inputs.empty());
		}

		/// Filter word compared against each entry's masks (see binding_included).
		BindingMask binding_filters = 0;

		/// Frame time of the most recent update, for manual_update calls.
		[[nodiscard]] VirtualTime time() const noexcept { return m_time; }

		/// Every registered virtual input, in registration order.
		[[nodiscard]] std::span<VirtualInput* const> inputs() const noexcept { return m_inputs; }

		/// Updates all active virtual inputs against the current Input state and
		/// dispatches gamepad connect/disconnect notifications to devices.
		void update(const Input& input, u64 now_ns) noexcept
		{
			m_time = {.now_ns = now_ns, .previous_ns = m_time.now_ns};

			notify_gamepad_changes(input);

			// Registration order means a VirtualDevice updates (and re-targets
			// its controller index) before the inputs it owns. Index loop so
			// inputs created from hooks are updated this same frame; destroying
			// inputs from within update is not supported.
			for (size_t i = 0; i < m_inputs.size(); ++i)
			{
				if (m_inputs[i]->active)
					m_inputs[i]->update(input, m_time, binding_filters);
			}
		}

	private:
		friend class VirtualInput;
		friend class VirtualDevice;

		void register_input(VirtualInput* input) { m_inputs.push_back(input); }
		void unregister_input(VirtualInput* input) noexcept { std::erase(m_inputs, input); }

		void register_device(VirtualDevice* device) { m_devices.push_back(device); }
		void unregister_device(VirtualDevice* device) noexcept { std::erase(m_devices, device); }

		void notify_gamepad_changes(const Input& input) noexcept;

		Vector<VirtualInput*> m_inputs;
		Vector<VirtualDevice*> m_devices;
		VirtualTime m_time{};
	};

	/**
	 * A virtual button ("jump", "confirm"), fed by an ActionBindingSet.
	 * Adds input buffering, press consumption, and hold-to-repeat on top of
	 * the raw bindings.
	 */
	class VirtualAction final : public VirtualInput
	{
	public:
		VirtualAction(
			VirtualInputs& owner, StringView name, ActionBindingSet set, u32 controller_index = 0, u64 buffer_ns = 0)
			: VirtualInput{owner, name, controller_index}, buffer_ns{buffer_ns}, m_set{std::move(set)}
		{
		}

		VirtualAction(VirtualInputs& owner, StringView name, u32 controller_index = 0, u64 buffer_ns = 0)
			: VirtualAction{owner, name, ActionBindingSet{}, controller_index, buffer_ns}
		{
		}

		/// Hold-to-repeat configuration. A zero interval disables repeating
		/// (the default, as in Foster); assign RepeatConfig{} for engine defaults.
		RepeatConfig repeat{.delay_ns = 0, .interval_ns = 0};

		/// Presses stay latched as pressed() for this long (input buffering),
		/// letting slightly-early presses still count, e.g. jump before landing.
		u64 buffer_ns = 0;

		/// The bindings driving this action; mutable for rebinding UI.
		[[nodiscard]] ActionBindingSet& set() noexcept { return m_set; }
		[[nodiscard]] const ActionBindingSet& set() const noexcept { return m_set; }

		/// Pressed this frame (includes buffered and repeated presses).
		[[nodiscard]] bool pressed() const noexcept { return m_pressed; }

		/// Currently held down.
		[[nodiscard]] bool down() const noexcept { return m_down; }

		/// Released this frame.
		[[nodiscard]] bool released() const noexcept { return m_released; }

		/// Whether this frame's pressed() came from hold-to-repeat.
		[[nodiscard]] bool repeated() const noexcept { return m_repeated; }

		/// Whether the current press was consumed.
		[[nodiscard]] bool press_consumed() const noexcept { return m_press_consumed; }

		/// Value in [0, 1]; 0 or 1 for buttons, analog for axis bindings.
		[[nodiscard]] f32 value() const noexcept { return m_value; }

		/// When the action was last genuinely pressed (never by repeating).
		[[nodiscard]] u64 last_press_ns() const noexcept { return m_last_press_ns; }

		/// Consumes the current press: pressed() reports false until a new press.
		/// Returns whether there was a press to consume. Consuming also discards
		/// any pending buffered press.
		bool consume_press() noexcept
		{
			if (!m_pressed)
				return false;

			m_pressed		 = false;
			m_press_consumed = true;
			return true;
		}

		/// Drives the action from a single bool instead of its bindings (touch
		/// controls, replays, ...). Set active to false and call this instead.
		/// Buffering and repeat still apply. (Unlike Foster, this also updates
		/// value(), which upstream leaves stale.)
		void manual_update(VirtualTime time, bool down) noexcept
		{
			m_pressed  = down && !m_down;
			m_released = !down && m_down;
			m_down	   = down;
			m_value	   = down ? 1.0f : 0.0f;
			m_repeated = false;

			apply_buffer_and_repeat(time, down);
		}

		/// Zeroes the action's state for this frame.
		void clear() noexcept
		{
			m_pressed		 = false;
			m_released		 = false;
			m_press_consumed = true;
			m_down			 = false;
			m_repeated		 = false;
			m_value			 = 0.0f;
		}

	private:
		void update(const Input& input, VirtualTime time, BindingMask filters) noexcept override
		{
			const BindingState state = m_set.state(input, controller_index(), filters);

			m_pressed  = state.pressed;
			m_released = state.released;
			m_down	   = state.down;
			m_value	   = state.value;
			m_repeated = false;

			apply_buffer_and_repeat(time, m_down);
		}

		void apply_buffer_and_repeat(VirtualTime time, bool down) noexcept
		{
			if (m_pressed)
			{
				// A fresh press restarts the buffer window and repeat timer.
				m_press_consumed = false;
				m_last_press_ns	 = time.now_ns;
			}
			else if (!m_press_consumed && m_last_press_ns > 0 && time.now_ns - m_last_press_ns < buffer_ns)
			{
				m_pressed = true;
			}

			if (!m_pressed && down &&
				input_detail::repeated(false, true, m_last_press_ns, time.previous_ns, time.now_ns, repeat))
			{
				m_pressed  = true;
				m_repeated = true;
			}
		}

		ActionBindingSet m_set;

		bool m_pressed		  = false;
		bool m_released		  = false;
		bool m_down			  = false;
		bool m_repeated		  = false;
		bool m_press_consumed = false;

		f32 m_value			= 0.0f;
		u64 m_last_press_ns = 0;
	};

	/**
	 * A virtual 1D axis fed by an AxisBindingSet, with optional keyboard-style
	 * repeat pulses on the held direction (useful for menu navigation).
	 */
	class VirtualAxis final : public VirtualInput
	{
	public:
		VirtualAxis(VirtualInputs& owner, StringView name, AxisBindingSet set, u32 controller_index = 0)
			: VirtualInput{owner, name, controller_index}, m_set{std::move(set)}
		{
		}

		VirtualAxis(VirtualInputs& owner, StringView name, u32 controller_index = 0)
			: VirtualAxis{owner, name, AxisBindingSet{}, controller_index}
		{
		}

		/// Hold-to-repeat configuration; a zero interval disables repeating.
		RepeatConfig repeat{.delay_ns = 0, .interval_ns = 0};

		/// The bindings driving this axis; mutable for rebinding UI.
		[[nodiscard]] AxisBindingSet& set() noexcept { return m_set; }
		[[nodiscard]] const AxisBindingSet& set() const noexcept { return m_set; }

		/// Current value in [-1, 1].
		[[nodiscard]] f32 value() const noexcept { return m_value; }

		/// Current value snapped to -1, 0, or +1.
		[[nodiscard]] i32 int_value() const noexcept { return m_int_value; }

		/// Sign of a press this frame, or 0 (includes repeat pulses).
		[[nodiscard]] i32 pressed_sign() const noexcept { return m_pressed_sign; }

		[[nodiscard]] bool pressed() const noexcept { return m_pressed_sign != 0; }
		[[nodiscard]] bool pressed_negative() const noexcept { return m_pressed_sign < 0; }
		[[nodiscard]] bool pressed_positive() const noexcept { return m_pressed_sign > 0; }

		/// Whether this frame's pressed_sign() came from hold-to-repeat.
		[[nodiscard]] bool repeated() const noexcept { return m_repeated; }

		/// When the axis was last genuinely pressed (never by repeating).
		[[nodiscard]] u64 last_press_ns() const noexcept { return m_last_press_ns; }

		/// Drives the axis from a single float instead of its bindings.
		/// Set active to false and call this instead.
		void manual_update(VirtualTime time, f32 value) noexcept
		{
			const i32 previous = m_int_value;

			m_value		   = value;
			m_int_value	   = sign_of(value);
			m_pressed_sign = (m_int_value != 0 && m_int_value != previous) ? m_int_value : 0;

			apply_repeat(time);
		}

		/// Zeroes the axis state for this frame.
		void clear() noexcept
		{
			m_value		   = 0.0f;
			m_int_value	   = 0;
			m_pressed_sign = 0;
		}

	private:
		void update(const Input& input, VirtualTime time, BindingMask filters) noexcept override
		{
			m_value		   = m_set.value(input, controller_index(), filters);
			m_int_value	   = sign_of(m_value);
			m_pressed_sign = m_set.pressed_sign(input, controller_index(), filters);

			apply_repeat(time);
		}

		void apply_repeat(VirtualTime time) noexcept
		{
			m_repeated = false;

			if (m_int_value == 0)
			{
				m_last_down_sign = 0;
			}
			else if (m_pressed_sign != 0)
			{
				m_last_down_sign = m_pressed_sign;
				m_last_press_ns	 = time.now_ns;
			}
			else if (
				m_last_down_sign == m_int_value && m_last_down_sign != 0 &&
				input_detail::repeated(false, true, m_last_press_ns, time.previous_ns, time.now_ns, repeat))
			{
				// Still holding the direction that was last pressed: pulse it.
				m_pressed_sign = m_last_down_sign;
				m_repeated	   = true;
			}
		}

		AxisBindingSet m_set;

		f32 m_value			 = 0.0f;
		i32 m_int_value		 = 0;
		i32 m_pressed_sign	 = 0;
		i32 m_last_down_sign = 0;
		bool m_repeated		 = false;
		u64 m_last_press_ns	 = 0;
	};

	/**
	 * A virtual 2D stick fed by a StickBindingSet.
	 * Convention: +X = right, +Y = down (up presses read negative Y).
	 */
	class VirtualStick final : public VirtualInput
	{
	public:
		VirtualStick(VirtualInputs& owner, StringView name, StickBindingSet set, u32 controller_index = 0)
			: VirtualInput{owner, name, controller_index}, m_set{std::move(set)}
		{
		}

		VirtualStick(VirtualInputs& owner, StringView name, u32 controller_index = 0)
			: VirtualStick{owner, name, StickBindingSet{}, controller_index}
		{
		}

		/// The bindings driving this stick; mutable for rebinding UI.
		[[nodiscard]] StickBindingSet& set() noexcept { return m_set; }
		[[nodiscard]] const StickBindingSet& set() const noexcept { return m_set; }

		/// Current value; each component in [-1, 1].
		[[nodiscard]] glm::vec2 value() const noexcept { return m_value; }

		/// Current value with each component snapped to -1, 0, or +1.
		[[nodiscard]] glm::ivec2 int_value() const noexcept { return m_int_value; }

		[[nodiscard]] bool pressed_left() const noexcept { return m_pressed_left; }
		[[nodiscard]] bool pressed_right() const noexcept { return m_pressed_right; }
		[[nodiscard]] bool pressed_up() const noexcept { return m_pressed_up; }
		[[nodiscard]] bool pressed_down() const noexcept { return m_pressed_down; }

		/// Drives the stick from a single vector instead of its bindings.
		/// Set active to false and call this instead.
		void manual_update(glm::vec2 value) noexcept
		{
			const glm::ivec2 previous = m_int_value;

			m_value		= value;
			m_int_value = {sign_of(value.x), sign_of(value.y)};

			m_pressed_left	= m_int_value.x < 0 && previous.x >= 0;
			m_pressed_right = m_int_value.x > 0 && previous.x <= 0;
			m_pressed_up	= m_int_value.y < 0 && previous.y >= 0;
			m_pressed_down	= m_int_value.y > 0 && previous.y <= 0;
		}

		/// Zeroes the stick state for this frame.
		void clear() noexcept
		{
			m_value		= {0.0f, 0.0f};
			m_int_value = {0, 0};

			m_pressed_left	= false;
			m_pressed_right = false;
			m_pressed_up	= false;
			m_pressed_down	= false;
		}

	private:
		void update(const Input& input, VirtualTime, BindingMask filters) noexcept override
		{
			m_value		= m_set.value(input, controller_index(), filters);
			m_int_value = {sign_of(m_value.x), sign_of(m_value.y)};

			m_pressed_left	= false;
			m_pressed_right = false;
			m_pressed_up	= false;
			m_pressed_down	= false;

			for (const StickBindingSet::Entry& entry : m_set.entries())
			{
				if (!binding_included(entry.masks, filters))
					continue;

				m_pressed_left	|= entry.left.state(input, controller_index()).pressed;
				m_pressed_right |= entry.right.state(input, controller_index()).pressed;
				m_pressed_up	|= entry.up.state(input, controller_index()).pressed;
				m_pressed_down	|= entry.down.state(input, controller_index()).pressed;
			}
		}

		StickBindingSet m_set;

		glm::vec2 m_value{0.0f, 0.0f};
		glm::ivec2 m_int_value{0, 0};

		bool m_pressed_left	 = false;
		bool m_pressed_right = false;
		bool m_pressed_up	 = false;
		bool m_pressed_down	 = false;
	};

	/**
	 * A collection of virtual inputs sharing one controller index, so a whole
	 * player's controls can be re-targeted to a different gamepad at once.
	 *
	 * Inputs created through add_action/add_axis/add_stick are owned by the
	 * device and have their controller index managed by it; do not set their
	 * index manually. Subclass to hook index/connection changes.
	 */
	class VirtualDevice : public VirtualInput
	{
	public:
		enum class IndexMode : u8
		{
			/// Controller index is assigned manually.
			Manual,

			/// Follow the most recently used connected gamepad.
			AutomaticLatest,
		};

		VirtualDevice(VirtualInputs& owner, StringView name, u32 controller_index = 0)
			: VirtualInput{owner, name, controller_index}
		{
			VirtualInput::owner().register_device(this);
		}

		~VirtualDevice() override
		{
			// Children unregister themselves when m_children destructs below.
			owner().unregister_device(this);
		}

		/// How the device selects its controller index.
		IndexMode index_mode = IndexMode::Manual;

		/// The inputs owned by this device, in creation order.
		[[nodiscard]] std::span<const std::unique_ptr<VirtualInput>> inputs() const noexcept { return m_children; }

		/// True while the assigned gamepad was used more recently than the
		/// keyboard/mouse - e.g. to pick button prompt art. Mirrors
		/// Input::gamepad_in_use for this device's pad; refreshed each update.
		[[nodiscard]] bool is_gamepad_latest() const noexcept { return m_gamepad_latest; }

		void set_controller_index(u32 index) noexcept override
		{
			// With AutomaticLatest the device owns the index; switch to Manual.
			EMBER_ASSERT(index_mode == IndexMode::Manual);
			apply_controller_index(index);
		}

		/// First owned input with the given name, or nullptr. Case sensitive.
		[[nodiscard]] VirtualInput* find(StringView name) const noexcept
		{
			for (const std::unique_ptr<VirtualInput>& child : m_children)
			{
				if (child->name() == name)
					return child.get();
			}

			return nullptr;
		}

		/// Adds a VirtualAction owned by this device.
		VirtualAction& add_action(StringView name, ActionBindingSet set, u64 buffer_ns = 0)
		{
			return add_child<VirtualAction>(name, std::move(set), controller_index(), buffer_ns);
		}

		VirtualAction& add_action(StringView name, u64 buffer_ns = 0)
		{
			return add_action(name, ActionBindingSet{}, buffer_ns);
		}

		/// Adds a VirtualAxis owned by this device.
		VirtualAxis& add_axis(StringView name, AxisBindingSet set = {})
		{
			return add_child<VirtualAxis>(name, std::move(set), controller_index());
		}

		/// Adds a VirtualStick owned by this device.
		VirtualStick& add_stick(StringView name, StickBindingSet set = {})
		{
			return add_child<VirtualStick>(name, std::move(set), controller_index());
		}

	protected:
		/// Called when the controller index changes (either mode).
		virtual void on_controller_index_changed() {}

		/// Called when a gamepad connects on this device's index.
		virtual void on_controller_connected() {}

		/// Called when the gamepad on this device's index disconnects.
		virtual void on_controller_disconnected() {}

	private:
		friend class VirtualInputs;

		void update(const Input& input, VirtualTime, BindingMask) noexcept override
		{
			if (index_mode == IndexMode::AutomaticLatest)
			{
				const std::span<const Gamepad> gamepads = input.gamepads();

				u32 latest = 0;
				for (u32 i = 1; i < gamepads.size(); ++i)
				{
					if (gamepads[i].connected() && gamepads[i].input_timestamp() > gamepads[latest].input_timestamp())
						latest = i;
				}

				apply_controller_index(latest);
			}

			const Gamepad& gamepad		= input.gamepads()[controller_index()];
			const u64 keyboard_or_mouse = std::max(input.keyboard().input_timestamp(), input.mouse().input_timestamp());

			m_gamepad_latest = gamepad.connected() && gamepad.input_timestamp() > keyboard_or_mouse;
		}

		void apply_controller_index(u32 index) noexcept
		{
			if (controller_index() == index)
				return;

			VirtualInput::set_controller_index(index);

			for (const std::unique_ptr<VirtualInput>& child : m_children)
				child->set_controller_index(index);

			on_controller_index_changed();
		}

		template <typename T, typename... Args> T& add_child(Args&&... args)
		{
			auto child = std::make_unique<T>(owner(), std::forward<Args>(args)...);
			T& result  = *child;

			m_children.push_back(std::move(child));
			return result;
		}

		Vector<std::unique_ptr<VirtualInput>> m_children;
		bool m_gamepad_latest = false;
	};

	inline VirtualInput::VirtualInput(VirtualInputs& owner, StringView name, u32 controller_index)
		: m_owner{&owner}, m_name{name}, m_controller_index{controller_index}
	{
		EMBER_ASSERT(controller_index < InputState::MAX_GAMEPADS);
		m_owner->register_input(this);
	}

	inline VirtualInput::~VirtualInput() { m_owner->unregister_input(this); }

	inline void VirtualInputs::notify_gamepad_changes(const Input& input) noexcept
	{
		for (u32 slot = 0; slot < InputState::MAX_GAMEPADS; ++slot)
		{
			const bool connected	 = input.state().gamepads()[slot].connected();
			const bool was_connected = input.last_state().gamepads()[slot].connected();

			if (connected == was_connected)
				continue;

			for (size_t i = 0; i < m_devices.size(); ++i)
			{
				VirtualDevice* device = m_devices[i];
				if (device->controller_index() != slot)
					continue;

				if (connected)
					device->on_controller_connected();
				else
					device->on_controller_disconnected();
			}
		}
	}
}
