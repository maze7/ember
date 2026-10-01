#pragma once

#include <ember/net/command_stream.h>

namespace ember::net
{
	struct TimeDilationDef
	{
		f32 margin		   = 0.5f;	// ticks of headroom wanted on top of the spread
		f32 spread		   = 3.0f;	// arrival deviations to stay clear of: jitter, frame pacing, loss
		f32 deadband	   = 0.5f;	// ticks of error ignored, so the clock settles instead of hunting
		f32 gain		   = 0.02f; // dilation per tick of error past the deadband
		f32 max_dilation   = 0.05f; // the fastest and slowest the clock runs: 16ms steps taken as 15.2 at most
		f32 snap_behind	   = 4.0f;	// ticks late past which the clock jumps forward instead of catching up
		f32 snap_ahead	   = 30.0f; // ticks early past which it jumps back instead of slowing down
		u32 settle_reports = 2;		// reports of a new epoch to see before another jump
	};

	/**
	 * Steers how far ahead of the server the client runs.
	 *
	 * The client wants each command to reach the server just before its tick runs: earlier is input
	 * latency for nothing, later is a tick on a stand-in and mispredictoin. The server measures how
	 * early commands arrive and reports it (CommandTiming); this turns that into the rate the commands
	 * are made earlier; too early: a little slower. The aim is a margin plus a few deviations of the
	 * arrival spread, so a jittery or lossy link earns a deeper buffer and a clean one runs close.
	 *
	 * Small errors are corrected by dilation, a few percent at most, which nobody sees. A large one
	 * (a hitch, a route change) would take seconds that way, so the clock jumps instead: forward when
	 * it fell behind (the server runs the skipped ticks on stand-ins), back when it got far ahead (sent
	 * ticks are replayed form CommandSender::find). A juimp starts a new epoch; commands carry it, the
	 * server restarts its measurement when it changes, and reports about an older epoch are ignored, so
	 * the clock never reacts twice to one error.
	 */
	class TimeDilation final
	{
	public:
		explicit TimeDilation(const TimeDilationDef& def = {}) noexcept : m_def(def) {}

		/** Back to real time and epoch 0, for a new session. */
		void reset() noexcept;

		/** Takes the server's latest measurement. Reports about an earlier epoch are ignored. */
		void report(const CommandTiming& timing) noexcept;

		/** The rate for the client's tick clock: 1 is real time, 1.05 is 5% fast. */
		[[nodiscard]] f32 time_scale() const noexcept { return m_scale; }

		/** Ticks to move the client's clock by now: positive forward. Zero almost always; reading clears it. */
		[[nodiscard]] i32 take_jump() noexcept;

		/** The epoch commands are sent with: 0 at the start, one more after every jump. */
		[[nodiscard]] u8 epoch() const noexcept { return m_epoch; }

		/** How early commands are aimed to arrive, in ticks, and how far off the last report was. */
		[[nodiscard]] f32 aim() const noexcept { return m_aim; }
		[[nodiscard]] f32 error() const noexcept { return m_error; }

	private:
		TimeDilationDef m_def;
		f32 m_scale	  = 1.0f;
		f32 m_aim	  = 0.0f;
		f32 m_error	  = 0.0f;
		i32 m_jump	  = 0;
		u32 m_reports = 0; // reports seen in this epoch
		u8 m_epoch	  = 0;
	};

	/**
	 * Where a joining client starts its clock. server_tick is the tick the server said it was on, half
	 * a round trip old by the time it arrives, and the client's first command needs the other half to
	 * get back: so that tick, plus the round trip, plus the headroom TimeDilation would aim for before
	 * it has measured anything. Dilation fine tunes from there.
	 */
	[[nodiscard]] Tick initial_client_tick(Tick server_tick, f64 rtt, f64 tick_seconds,
										   const TimeDilationDef& def = {}) noexcept;
}
