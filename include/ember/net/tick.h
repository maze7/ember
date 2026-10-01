#pragma once

#include <ember/core/common.h>

#include <array>
#include <concepts>

namespace ember::net
{
	using Tick = u32;

	constexpr inline Tick NO_TICK = 0;

	/**
	 * The last N ticks of something, addressed by tick: commands by the tick they drive, predicted
	 * states by the tick they end, a hitbox by the tick it stood there. Writing tick t claims slot
	 * t % N and evicts whatever tick held it; finding a tick checks the slot still holds that tick,
	 * so a lookup outside the window misses instead of returning another tick's value.
	 *
	 * Ticks may be written in any order and with gaps. Values are stored in place and reused; a
	 * write hands back the slot holding its previous contents, for the caller to overwrite.
	 */
	template <class T, u32 N> class TickRing final
	{
		static_assert(N != 0);

	public:
		static constexpr u32 CAPACITY = N;

		/** Claims the slot for tick, evicting whatever it held, and returns it for filling. */
		[[nodiscard]] T& write(Tick tick) noexcept
		{
			EMBER_ASSERT(tick != NO_TICK);

			Slot& slot = m_slots[tick % N];
			slot.tick  = tick;
			return slot.value;
		}

		template <class U>
			requires std::assignable_from<T&, U&&>
		T& write(Tick tick, U&& value) noexcept
		{
			T& slot = write(tick);
			slot	= static_cast<U&&>(value);
			return slot;
		}

		[[nodiscard]] T* find(Tick tick) noexcept
		{
			Slot& slot = m_slots[tick % N];
			return tick != NO_TICK && slot.tick == tick ? &slot.value : nullptr;
		}

		[[nodiscard]] const T* find(Tick tick) const noexcept
		{
			const Slot& slot = m_slots[tick % N];
			return tick != NO_TICK && slot.tick == tick ? &slot.value : nullptr;
		}

		[[nodiscard]] bool contains(Tick tick) const noexcept { return find(tick) != nullptr; }

		/** Forgets the tick, if the ring holds it. */
		void erase(Tick tick) noexcept
		{
			Slot& slot = m_slots[tick % N];
			if (slot.tick == tick)
				slot.tick = NO_TICK;
		}

		/** Forgets every tick. Values stay in their slots until overwritten. */
		void clear() noexcept
		{
			for (Slot& slot : m_slots)
				slot.tick = NO_TICK;
		}

	private:
		struct Slot
		{
			Tick tick = NO_TICK;
			T value	  = {};
		};

		std::array<Slot, N> m_slots = {};
	};
}
