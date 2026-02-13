#pragma once

#include "common.h"

namespace Ember
{
	class Time
	{
	public:
		static constexpr u64 TICKS_PER_SECOND = 1000 * 100;

		static void tick();

		static u64 ticks() { return m_ticks; }

		static double seconds() { return static_cast<double>(m_ticks) / TICKS_PER_SECOND; }

		static float delta() { return m_delta; }

		static bool on_interval(double time, float delta, float interval, float offset);

		static bool on_interval(float delta, float interval, float offset);

		static bool on_interval(float interval, float offset = 0);

		static bool on_time(double time, double timestep);

		static bool between_interval(double time, float interval, float offset);

		static bool between_interval(float interval, float offset = 0);

	private:
		// delta time from last frame
		static float m_delta;

		// uptime, in ticks, to the start of the current frame
		static u64 m_ticks;

		// uptime, in ticks, to the start of the previous frame
		static u64 m_previous_ticks;

		static u64 m_init_tick;
	};
}

