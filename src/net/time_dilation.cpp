#include <ember/net/time_dilation.h>

#include <algorithm>
#include <cmath>

namespace ember::net
{
	namespace
	{
		constexpr f32 PRIOR_DEVIATION = 0.5f; // ticks of spread assumed before anything is measured
	}

	void TimeDilation::reset() noexcept
	{
		m_scale	  = 1.0f;
		m_aim	  = 0.0f;
		m_error	  = 0.0f;
		m_jump	  = 0;
		m_reports = 0;
		m_epoch	  = 0;
	}

	void TimeDilation::report(const CommandTiming& timing) noexcept
	{
		if (timing.epoch != m_epoch)
			return;

		++m_reports;

		m_aim	= m_def.margin + m_def.spread * timing.deviation;
		m_error = timing.mean - m_aim;

		// Too far off to steer back: jump, and start an epoch so the old measurement is not acted on again.
		const bool settled = m_reports >= m_def.settle_reports;
		if (settled && (m_error < -m_def.snap_behind || m_error > m_def.snap_ahead))
		{
			m_jump += -static_cast<i32>(std::lround(m_error));
			m_scale	  = 1.0f;
			m_reports = 0;
			++m_epoch;
			return;
		}

		const f32 beyond =
			std::abs(m_error) <= m_def.deadband ? 0.0f : m_error - std::copysign(m_def.deadband, m_error);
		m_scale = 1.0f - std::clamp(m_def.gain * beyond, -m_def.max_dilation, m_def.max_dilation);
	}

	i32 TimeDilation::take_jump() noexcept
	{
		const i32 jump = m_jump;
		m_jump		   = 0;
		return jump;
	}

	Tick initial_client_tick(Tick server_tick, f64 rtt, f64 tick_seconds, const TimeDilationDef& def) noexcept
	{
		EMBER_ASSERT(tick_seconds > 0.0);

		// A whole number of ticks stays whole: 0.1 s / (1/60 s) is 6.000000000000001 in doubles.
		const f64 headroom = def.margin + def.spread * PRIOR_DEVIATION;
		return server_tick + static_cast<Tick>(std::ceil(std::max(rtt, 0.0) / tick_seconds + headroom - 1e-9));
	}
}
