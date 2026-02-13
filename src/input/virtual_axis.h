#pragma once

#include "input.h"
#include "virtual_input.h"

namespace Ember
{
	class VirtualAxis
	{
	public:
		enum class Overlap
		{
			/// Uses whichever input was pressed most recently
			TakeNewer,

			/// Uses whichever input was pressed first
			TakeOlder,

			/// Contradicting inputs cancel each other out
			CancelOut,
		};

		/// Returns the current overlap behaviour
		Overlap overlap;

		/// constructor
		explicit VirtualAxis(Input& input, int controller = 0);

		/// destructor
		~VirtualAxis();

		/// @returns Reference to negative VirtualAction
		[[nodiscard]] VirtualInput& negative() {
			return m_negative;
		}

		/// @returns Reference to positive VirtualAction
		[[nodiscard]] VirtualInput& positive() {
			return m_positive;
		}

		/**
		 * Gets the current value of the Axis from the provided Input class
		 * @return float value of the VirtualAxis
		 */
		[[nodiscard]] float value() const;

		/**
		 * Adds a Keyboard Key mapping
		 * @param negative Negative Key
		 * @param positive Positive Key
		 * @return Reference to this, for function chaining
		 */
		VirtualAxis& add(Key negative, Key positive);

		/**
		 * Adds a MouseButton mapping
		 * @param negative Negative MouseButton
		 * @param positive Positive MouseButton
		 * @return Reference to this, for function chaining
		 */
		VirtualAxis& add(MouseButton negative, MouseButton positive);

		/**
		 * Adds a ControllerButton mapping
		 * @param negative Negative ControllerButton
		 * @param positive Positive ControllerButton
		 * @return Reference to this, for function chaining
		 */
		VirtualAxis& add(ControllerButton negative, ControllerButton positive);

		/**
		 * Adds a Controller Axis mapping
		 * @param axis Controller Axis to be used
		 * @param deadzone Ignored threshold (from zero) in each direction on
		 * the given Axis
		 * @return Reference to this, for function chaining
		 */
		VirtualAxis& add(Axis axis, float deadzone = 0);

	private:
		Input& m_input;
		VirtualInput m_negative;
		VirtualInput m_positive;
	};
}
