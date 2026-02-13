#include "core/time.h"
#include "SDL3/SDL_timer.h"

using namespace Ember;

u64 Time::m_ticks = 0;
u64 Time::m_previous_ticks = 0;
u64 Time::m_init_tick = 0;
float Time::m_delta = 0;

void Time::tick() {
	u64 counter = SDL_GetPerformanceCounter();
	u64 per_second = SDL_GetPerformanceFrequency();

	// Convert SDL ticks to custom tick rate
	u64 current_ticks = static_cast<u64>(counter * (TICKS_PER_SECOND / static_cast<double>(per_second)));

	// Set the initial tick reference on the first call
	if(m_init_tick == 0) {
		m_init_tick = current_ticks;
	}

	// Adjust ticks relative to the initial tick
	m_ticks = current_ticks - m_init_tick;
	m_delta = static_cast<float>(m_ticks - m_previous_ticks) / TICKS_PER_SECOND;
	m_previous_ticks = m_ticks;
}

bool Time::on_interval(double time, float delta, float interval, float offset) {
	auto last = static_cast<u64>((time - offset - delta) / interval);
	auto next = static_cast<u64>((time - offset) / interval);
	return last < next;
}

bool Time::on_interval(float delta, float interval, float offset) {
	return on_interval(seconds(), delta, interval, offset);
}

bool Time::on_interval(float interval, float offset) {
	return on_interval(seconds(), delta(), interval, offset);
}

bool Time::on_time(double time, double timestamp) {
	float c = static_cast<float>(time) - delta();
	return time >= timestamp && c < timestamp;
}

bool Time::between_interval(double time, float interval, float offset) {
	static const auto modf = [](double x, double m) { return x - (int)(x / m) * m; };

	return modf(time - offset, ((double)interval) * 2) >= interval;
}

bool Time::between_interval(float interval, float offset) {
	return between_interval(seconds(), interval, offset);
}
