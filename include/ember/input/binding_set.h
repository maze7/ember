#pragma once

#include "ember/memory/common.h"
#include <ember/input/binding.h>
#include <ember/memory/memory.h>

#include <initializer_list>

namespace ember
{
	/**
	 * Bindings for a single digital action ("jump", "confirm", ...).
	 *
	 * Entries combine permissively: flags OR together, value and timestamp take
	 * the max, so any active entry can drive the action.
	 */
	class ActionBindingSet final
	{
	public:
		/// A single binding plus the filter masks it is subject to.
		struct Entry
		{
			Binding binding;
			BindingMask masks = 0;
		};

		ActionBindingSet() = default;

		ActionBindingSet(std::initializer_list<Binding> bindings)
		{
			m_entries.reserve(bindings.size());
			for (const Binding& binding : bindings)
				m_entries.push_back({.binding = binding});
		}

		[[nodiscard]] Vector<Entry>& entries() noexcept { return m_entries; }
		[[nodiscard]] const Vector<Entry>& entries() const noexcept { return m_entries; }

		/// Adds any Binding
		ActionBindingSet& add(Binding binding, BindingMask masks = 0)
		{
			m_entries.push_back({.binding = binding, .masks = masks});
			return *this;
		}

		/// Adds a keyboard Key mapping.
		ActionBindingSet& add(Key key, BindingMask masks = 0) { return add(Binding{KeyBinding{key}}, masks); }

		/// Adds keyboard Key mappings.
		ActionBindingSet& add(std::initializer_list<Key> keys, BindingMask masks = 0)
		{
			for (const Key key : keys)
				add(key, masks);
			return *this;
		}

		/// Adds a MouseButton mapping.
		ActionBindingSet& add(MouseButton button, BindingMask masks = 0)
		{
			return add(Binding{MouseButtonBinding{button}}, masks);
		}

		/// Adds MouseButton mappings.
		ActionBindingSet& add(std::initializer_list<MouseButton> buttons, BindingMask masks = 0)
		{
			for (const MouseButton button : buttons)
				add(button, masks);
			return *this;
		}

		/// Adds a GamepadButton mapping.
		ActionBindingSet& add(GamepadButton button, BindingMask masks = 0)
		{
			return add(Binding{GamepadButtonBinding{button}}, masks);
		}

		/// Adds GamepadButton mappings.
		ActionBindingSet& add(std::initializer_list<GamepadButton> buttons, BindingMask masks = 0)
		{
			for (const GamepadButton button : buttons)
				add(button, masks);
			return *this;
		}

		/// Adds one direction of a GamepadAxis (sign is +1 or -1).
		ActionBindingSet& add(GamepadAxis axis, i32 sign, f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(Binding{GamepadAxisBinding{axis, sign, deadzone}}, masks);
		}

		ActionBindingSet& add_left_stick_left(f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(GamepadAxis::LeftX, -1, deadzone, masks);
		}

		ActionBindingSet& add_left_stick_right(f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(GamepadAxis::LeftX, +1, deadzone, masks);
		}

		ActionBindingSet& add_left_stick_up(f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(GamepadAxis::LeftY, -1, deadzone, masks);
		}

		ActionBindingSet& add_left_stick_down(f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(GamepadAxis::LeftY, +1, deadzone, masks);
		}

		ActionBindingSet& add_right_stick_left(f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(GamepadAxis::RightX, -1, deadzone, masks);
		}

		ActionBindingSet& add_right_stick_right(f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(GamepadAxis::RightX, +1, deadzone, masks);
		}

		ActionBindingSet& add_right_stick_up(f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(GamepadAxis::RightY, -1, deadzone, masks);
		}

		ActionBindingSet& add_right_stick_down(f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(GamepadAxis::RightY, +1, deadzone, masks);
		}

		/// Samples the combined state of all entries passing the filters.
		[[nodiscard]] BindingState state(const Input& input, u32 device, BindingMask filters = 0) const noexcept
		{
			BindingState result{};

			for (const Entry& entry : m_entries)
			{
				if (!binding_included(entry.masks, filters))
					continue;

				const BindingState state = entry.binding.state(input, device);

				result.pressed		|= state.pressed;
				result.released		|= state.released;
				result.down			|= state.down;
				result.value		 = std::max(result.value, state.value);
				result.timestamp_ns	 = std::max(result.timestamp_ns, state.timestamp_ns);
			}

			return result;
		}

	private:
		Vector<Entry> m_entries{&memory::heap(MemoryTag::Input)};
	};

	/**
	 * Bindings for a 1D axis built from negative/positive binding pairs
	 * ("move x", "camera zoom", ...).
	 *
	 * Each entry resolves its own overlap; across entries, the strongest magnitude wins.
	 */
	class AxisBindingSet final
	{
	public:
		/// A negative/positive binding pair plus overlap behaviour and masks.
		struct Entry
		{
			Binding negative;
			Binding positive;
			BindingAxisOverlap overlap = BindingAxisOverlap::TakeNewer;
			BindingMask masks		   = 0;
		};

		AxisBindingSet() = default;

		AxisBindingSet(std::initializer_list<Entry> entries) : m_entries{&memory::heap(MemoryTag::Input), entries} {}

		[[nodiscard]] Vector<Entry>& entries() noexcept { return m_entries; }
		[[nodiscard]] const Vector<Entry>& entries() const noexcept { return m_entries; }

		/// Adds any negative/positive Binding pair.
		AxisBindingSet& add(Binding negative, Binding positive, BindingAxisOverlap overlap = {}, BindingMask masks = 0)
		{
			m_entries.push_back({.negative = negative, .positive = positive, .overlap = overlap, .masks = masks});
			return *this;
		}

		/// Adds a keyboard Key pair.
		AxisBindingSet& add(Key negative, Key positive, BindingAxisOverlap overlap = {}, BindingMask masks = 0)
		{
			return add(Binding{KeyBinding{negative}}, Binding{KeyBinding{positive}}, overlap, masks);
		}

		/// Adds a GamepadButton pair.
		AxisBindingSet&
		add(GamepadButton negative, GamepadButton positive, BindingAxisOverlap overlap = {}, BindingMask masks = 0)
		{
			return add(
				Binding{GamepadButtonBinding{negative}}, Binding{GamepadButtonBinding{positive}}, overlap, masks);
		}

		/// Adds both directions of a GamepadAxis.
		AxisBindingSet& add(GamepadAxis axis, f32 deadzone = 0.0f, BindingMask masks = 0)
		{
			return add(
				Binding{GamepadAxisBinding{axis, -1, deadzone}},
				Binding{GamepadAxisBinding{axis, +1, deadzone}},
				{},
				masks);
		}

		/// Current value of the axis in [-1, 1].
		[[nodiscard]] f32 value(const Input& input, u32 device, BindingMask filters = 0) const noexcept
		{
			f32 value = 0.0f;

			for (const Entry& entry : m_entries)
			{
				if (!binding_included(entry.masks, filters))
					continue;

				const f32 next = resolve_axis_overlap(
					entry.overlap, entry.negative.state(input, device), entry.positive.state(input, device));

				if (std::abs(next) > std::abs(value))
					value = next;
			}

			return value;
		}

		/// Sign of a press that happened this frame, or 0 if none. Only bindings
		/// freshly pressed this frame contribute.
		[[nodiscard]] i32 pressed_sign(const Input& input, u32 device, BindingMask filters = 0) const noexcept
		{
			f32 value = 0.0f;

			for (const Entry& entry : m_entries)
			{
				if (!binding_included(entry.masks, filters))
					continue;

				BindingState negative = entry.negative.state(input, device);
				BindingState positive = entry.positive.state(input, device);

				if (!negative.pressed)
					negative = {};
				if (!positive.pressed)
					positive = {};

				const f32 next = resolve_axis_overlap(entry.overlap, negative, positive);

				if (std::abs(next) > std::abs(value))
					value = next;
			}

			return sign_of(value);
		}

	private:
		Vector<Entry> m_entries{&memory::heap(MemoryTag::Input)};
	};

	/**
	 * Bindings for a 2D stick built from left/right/up/down binding quads.
	 *
	 * Convention matches sticks and window space: +X = right, +Y = down, so
	 * "up" bindings produce negative Y. Each entry resolves its own overlap;
	 * across entries, the largest magnitude wins.
	 */
	class StickBindingSet final
	{
	public:
		/// A left/right/up/down binding quad plus deadzone, overlap, and masks.
		struct Entry
		{
			Binding left;
			Binding right;
			Binding up;
			Binding down;

			/// Circular deadzone applied to the entry's combined value.
			f32 circular_deadzone = 0.0f;

			BindingAxisOverlap overlap = BindingAxisOverlap::TakeNewer;
			BindingMask masks		   = 0;
		};

		StickBindingSet() = default;

		StickBindingSet(std::initializer_list<Entry> entries) : m_entries{&memory::heap(MemoryTag::Input), entries} {}

		[[nodiscard]] Vector<Entry>& entries() noexcept { return m_entries; }
		[[nodiscard]] const Vector<Entry>& entries() const noexcept { return m_entries; }

		/// Adds any left/right/up/down Binding quad.
		StickBindingSet& add(Entry entry)
		{
			m_entries.push_back(std::move(entry));
			return *this;
		}

		/// Adds a keyboard Key quad.
		StickBindingSet&
		add(Key left, Key right, Key up, Key down, BindingAxisOverlap overlap = {}, BindingMask masks = 0)
		{
			// clang-format off
			return add({
				 .left	  = Binding{KeyBinding{left}},
				 .right	  = Binding{KeyBinding{right}},
				 .up	  = Binding{KeyBinding{up}},
				 .down	  = Binding{KeyBinding{down}},
				 .overlap = overlap,
				 .masks	  = masks
			});
			// clang-format on
		}

		/// Adds a GamepadButton quad.
		StickBindingSet&
		add(GamepadButton left,
			GamepadButton right,
			GamepadButton up,
			GamepadButton down,
			BindingAxisOverlap overlap = {},
			BindingMask masks		   = 0)
		{
			// clang-format off
			return add({
				.left	 = Binding{GamepadButtonBinding{left}},
				.right	 = Binding{GamepadButtonBinding{right}},
				.up		 = Binding{GamepadButtonBinding{up}},
				.down	 = Binding{GamepadButtonBinding{down}},
				.overlap = overlap,
				.masks	 = masks
			});
			// clang-format on
		}

		/// Adds both directions of two GamepadAxes with per axis deadzones.
		StickBindingSet&
		add(GamepadAxis x,
			f32 x_deadzone,
			GamepadAxis y,
			f32 y_deadzone,
			f32 circular_deadzone,
			BindingAxisOverlap overlap = {},
			BindingMask masks		   = 0)
		{
			return add({
				.left			   = Binding{GamepadAxisBinding{x, -1, x_deadzone}},
				.right			   = Binding{GamepadAxisBinding{x, +1, x_deadzone}},
				.up				   = Binding{GamepadAxisBinding{y, -1, y_deadzone}},
				.down			   = Binding{GamepadAxisBinding{y, +1, y_deadzone}},
				.circular_deadzone = circular_deadzone,
				.overlap		   = overlap,
				.masks			   = masks,
			});
		}

		/// Adds a GamepadAxis pair with a circular deadzone only.
		StickBindingSet&
		add(GamepadAxis x, GamepadAxis y, f32 circular_deadzone, BindingAxisOverlap overlap = {}, BindingMask masks = 0)
		{
			return add(x, 0.0f, y, 0.0f, circular_deadzone, overlap, masks);
		}

		/// Adds mouse motion, saturating at max_motion pixels of travel per frame.
		StickBindingSet& add_mouse_motion(f32 max_motion = 25.0f, BindingMask masks = 0)
		{
			return add({
				.left  = Binding{MouseMotionBinding{.axis = {1.0f, 0.0f}, .sign = -1, .max = max_motion}},
				.right = Binding{MouseMotionBinding{.axis = {1.0f, 0.0f}, .sign = +1, .max = max_motion}},
				.up	   = Binding{MouseMotionBinding{.axis = {0.0f, 1.0f}, .sign = -1, .max = max_motion}},
				.down  = Binding{MouseMotionBinding{.axis = {0.0f, 1.0f}, .sign = +1, .max = max_motion}},
				.masks = masks,
			});
		}

		StickBindingSet& add_arrow_keys(BindingAxisOverlap overlap = {}, BindingMask masks = 0)
		{
			return add(Key::Left, Key::Right, Key::Up, Key::Down, overlap, masks);
		}

		StickBindingSet& add_wasd(BindingAxisOverlap overlap = {}, BindingMask masks = 0)
		{
			return add(Key::A, Key::D, Key::W, Key::S, overlap, masks);
		}

		StickBindingSet& add_dpad(BindingAxisOverlap overlap = {}, BindingMask masks = 0)
		{
			return add(
				GamepadButton::Left, GamepadButton::Right, GamepadButton::Up, GamepadButton::Down, overlap, masks);
		}

		StickBindingSet& add_left_stick(f32 deadzone, BindingMask masks = 0)
		{
			return add(GamepadAxis::LeftX, GamepadAxis::LeftY, deadzone, {}, masks);
		}

		StickBindingSet& add_right_stick(f32 deadzone, BindingMask masks = 0)
		{
			return add(GamepadAxis::RightX, GamepadAxis::RightY, deadzone, {}, masks);
		}

		/// Current value of the stick; each component in [-1, 1], +Y = down.
		[[nodiscard]] glm::vec2 value(const Input& input, u32 device, BindingMask filters = 0) const noexcept
		{
			glm::vec2 value{0.0f, 0.0f};

			for (const Entry& entry : m_entries)
			{
				if (!binding_included(entry.masks, filters))
					continue;

				const glm::vec2 next{
					resolve_axis_overlap(
						entry.overlap, entry.left.state(input, device), entry.right.state(input, device)),
					resolve_axis_overlap(entry.overlap, entry.up.state(input, device), entry.down.state(input, device)),
				};

				if (entry.circular_deadzone > 0.0f &&
					glm::dot(next, next) < entry.circular_deadzone * entry.circular_deadzone)
					continue;

				if (glm::dot(next, next) > glm::dot(value, value))
					value = next;
			}

			return value;
		}

	private:
		Vector<Entry> m_entries{&memory::heap(MemoryTag::Input)};
	};
}
