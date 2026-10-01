#include <ember/net/loopback.h>
#include <ember/net/time_dilation.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::net;

	constexpr f64 DT	 = 1.0 / 60.0;
	constexpr u8 HELD	 = 1 << 0;
	constexpr u8 PRESSED = 1 << 1;

	struct Input
	{
		i8 move	   = 0;
		u8 buttons = 0;

		bool operator==(const Input&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_int(stream, move, -100, 100);
			serialize_bits(stream, buttons, 2);
			return true;
		}
	};

	void repeat_command(Input& input) { input.buttons &= ~PRESSED; }
	void merge_late_command(Input& next, const Input& late) { next.buttons |= late.buttons & PRESSED; }

	struct SessionDef
	{
		LinkConditions link;
		f64 frame		 = 1.0 / 144.0; // the client's render frame
		f64 frame_jitter = 0.002;
		u64 seed		 = 1;
		i32 start_error	 = 0; // ticks the client's first guess is off by
	};

	/**
	 * A server and one client ticking at 60 Hz over a loopback link. The server runs its ticks on
	 * time; the client runs frames, ticks a fixed step clock scaled by TimeDilation inside them (at
	 * most five ticks a frame, as the game's TickClock does), and sends one packet per tick.
	 */
	class Session
	{
	public:
		explicit Session(const SessionDef& def)
			: m_def(def), m_network(def.seed), m_server_end(m_network), m_client_end(m_network), m_random(def.seed)
		{
			m_server_end.set_conditions(def.link);
			m_client_end.set_conditions(def.link);
			m_latency = def.link.latency;

			EXPECT_TRUE(m_server_end.listen("server"));
			m_to_server = m_client_end.connect("server", 0.0).value();

			TransportEvent event;
			EXPECT_TRUE(m_server_end.poll(1.0, event));
			m_to_client = event.peer;
			EXPECT_TRUE(m_client_end.poll(1.0, event));

			// The client knows the round trip from its handshake and starts from initial_client_tick.
			m_client_tick = initial_client_tick(m_server_tick, 2.0 * def.link.latency, DT) + def.start_error;
		}

		void run_until(f64 end)
		{
			while (true)
			{
				const f64 next = std::min(m_next_frame, m_next_tick);
				if (next > end)
					return;

				if (m_next_tick <= m_next_frame)
					server_tick(m_next_tick);
				else
					client_frame(m_next_frame);
			}
		}

		void set_link(const LinkConditions& link)
		{
			m_server_end.set_conditions(link);
			m_client_end.set_conditions(link);
			m_latency = link.latency;
		}

		void hitch(f64 seconds) { m_hitch = seconds; }

		[[nodiscard]] u32 misses_between(f64 from, f64 to) const
		{
			return static_cast<u32>(
				std::count_if(m_misses.begin(), m_misses.end(), [&](f64 time) { return time >= from && time < to; }));
		}

		[[nodiscard]] u32 ticks_between(f64 from, f64 to) const
		{
			return static_cast<u32>(
				std::count_if(m_ticks.begin(), m_ticks.end(), [&](f64 time) { return time >= from && time < to; }));
		}

		/** Time scales the client ran at, one per frame, between two times. */
		[[nodiscard]] f64 mean_scale(f64 from, f64 to) const
		{
			f64 sum	  = 0.0;
			u32 count = 0;
			for (const auto& [time, scale] : m_scales)
			{
				if (time >= from && time < to)
				{
					sum += scale;
					++count;
				}
			}
			return count > 0 ? sum / count : 1.0;
		}

		[[nodiscard]] const CommandQueue& queue() const { return m_queue; }
		[[nodiscard]] const TimeDilation& dilation() const { return m_dilation; }
		[[nodiscard]] u32 jumps() const { return m_jumps; }
		[[nodiscard]] Tick lead() const { return m_client_tick - m_server_tick; }

		/**
		 * True when every press the client made reached the server exactly once: on its own tick, or
		 * on the first tick after its late arrival. Presses are seven ticks apart, so a window of
		 * LATE_WINDOW ticks never holds two. Presses made while the client was behind (from, to) are
		 * skipped: a burst of late commands folds its presses into one carry, which lands once.
		 */
		[[nodiscard]] bool every_press_landed_once(Tick from = NO_TICK, Tick to = NO_TICK) const
		{
			constexpr Tick LATE_WINDOW = 5;

			for (const Tick press : m_pressed)
			{
				if (press + LATE_WINDOW >= m_server_tick || (press >= from && press <= to))
					continue;

				u32 landed = 0;
				for (Tick tick = press; tick < press + LATE_WINDOW; ++tick)
					landed += m_seen.contains(tick) ? 1 : 0;

				if (landed != 1)
					return false;
			}

			return !m_pressed.empty() && no_press_invented();
		}

		/** Every press the server ran was made by the client, on that tick or shortly before. */
		[[nodiscard]] bool no_press_invented() const
		{
			for (const Tick seen : m_seen)
			{
				const auto made = m_pressed.upper_bound(seen);
				if (made == m_pressed.begin() || seen - *std::prev(made) > COMMAND_HISTORY)
					return false;
			}

			return m_seen.size() <= m_pressed.size();
		}

		[[nodiscard]] Tick server_tick() const { return m_server_tick; }

		/** How far ahead the client should be: one way to the server, plus the measured buffer. */
		[[nodiscard]] f32 expected_lead() const { return static_cast<f32>(m_latency / DT) + m_queue.timing().mean; }

	private:
		void server_tick(f64 now)
		{
			TransportEvent event;
			while (m_server_end.poll(now, event))
			{
				if (event.kind != TransportEventKind::Received)
					continue;

				PacketBuffer buffer;
				serialize::ReadStream stream;
				if (!packet_reader(stream, buffer, event.data))
					continue;

				Sequence sequence = 0;
				if (m_server_connection.read_header(stream, sequence, event.time) != Connection::Arrival::Fresh)
					continue;

				if (m_queue.read(stream, event.time))
					m_server_connection.acknowledge(sequence, static_cast<u32>(event.data.size()), event.time);
			}

			++m_server_tick;

			Input input;
			const TakenCommand taken = m_queue.take(m_server_tick, input, now);

			m_ticks.push_back(now);
			if (taken.source != CommandSource::Received)
				m_misses.push_back(now);
			if ((input.buttons & PRESSED) != 0)
				m_seen.insert(m_server_tick);

			PacketBuffer buffer;
			serialize::WriteStream stream = packet_writer(buffer);
			(void)m_server_connection.write_header(stream, now);
			write_command_timing(stream, m_queue);
			stream.Flush();
			m_server_end.send(m_to_client, written(buffer, stream), Delivery::Unreliable, now);

			PacketNotice notice;
			while (m_server_connection.take_notice(notice))
			{
			}

			m_next_tick += DT;
		}

		void client_frame(f64 now)
		{
			TransportEvent event;
			while (m_client_end.poll(now, event))
			{
				if (event.kind != TransportEventKind::Received)
					continue;

				PacketBuffer buffer;
				serialize::ReadStream stream;
				if (!packet_reader(stream, buffer, event.data))
					continue;

				Sequence sequence = 0;
				if (m_client_connection.read_header(stream, sequence, event.time) != Connection::Arrival::Fresh)
					continue;

				CommandTiming timing;
				bool present = false;
				if (!read_command_timing(stream, timing, present))
					continue;

				if (present)
					m_dilation.report(timing);

				m_client_connection.acknowledge(sequence, static_cast<u32>(event.data.size()), event.time);
			}

			PacketNotice notice;
			while (m_client_connection.take_notice(notice))
				m_sender.on_notice(notice);

			if (const i32 jump = m_dilation.take_jump(); jump != 0)
			{
				m_client_tick = static_cast<Tick>(static_cast<i64>(m_client_tick) + jump);
				++m_jumps;
			}

			const f64 frame = m_def.frame + m_def.frame_jitter * (2.0 * uniform() - 1.0) + m_hitch;
			m_hitch			= 0.0;

			m_scales.emplace_back(now, m_dilation.time_scale());
			m_accumulator += frame * m_dilation.time_scale();

			// The game's TickClock: at most five ticks a frame, the rest of a stall is dropped.
			u32 owed = static_cast<u32>(m_accumulator / DT);
			if (owed > 5)
			{
				owed		  = 5;
				m_accumulator = 0.0;
			}
			else
			{
				m_accumulator -= owed * DT;
			}

			for (u32 i = 0; i < owed; ++i)
			{
				++m_client_tick;

				const bool press  = m_client_tick % 7 == 0;
				const Input input = {.move	  = static_cast<i8>(m_client_tick % 50),
									 .buttons = static_cast<u8>(HELD | (press ? PRESSED : 0))};

				if (m_sender.newest() == NO_TICK || m_client_tick > m_sender.newest())
				{
					m_sender.push(m_client_tick, input, 7.25f);
					if (press)
						m_pressed.insert(m_client_tick);
				}

				PacketBuffer buffer;
				serialize::WriteStream stream = packet_writer(buffer);
				const Sequence sequence		  = m_client_connection.write_header(stream, now);
				m_sender.write(stream, sequence, m_dilation.epoch());
				stream.Flush();
				m_client_end.send(m_to_server, written(buffer, stream), Delivery::Unreliable, now);
			}

			m_next_frame = now + frame;
		}

		[[nodiscard]] f64 uniform() { return std::uniform_real_distribution<f64>(0.0, 1.0)(m_random); }

		SessionDef m_def;
		LoopbackNetwork m_network;
		LoopbackTransport m_server_end;
		LoopbackTransport m_client_end;
		PeerId m_to_server = NO_PEER;
		PeerId m_to_client = NO_PEER;

		Connection m_server_connection;
		Connection m_client_connection;
		CommandSender m_sender{command_codec<Input>()};
		CommandQueue m_queue{command_codec<Input>(), {.tick_seconds = DT}};
		TimeDilation m_dilation;

		Tick m_server_tick = 1000;
		Tick m_client_tick = NO_TICK;
		f64 m_next_tick	   = 1.0;
		f64 m_next_frame   = 1.0;
		f64 m_accumulator  = 0.0;
		f64 m_hitch		   = 0.0;
		std::mt19937_64 m_random;

		std::vector<f64> m_misses;
		std::vector<f64> m_ticks;
		std::vector<std::pair<f64, f32>> m_scales;
		std::set<Tick> m_pressed;
		std::set<Tick> m_seen;
		f64 m_latency = 0.0;
		u32 m_jumps	  = 0;
	};

	TEST(SessionClock, ACleanLinkSettlesWithNoLateCommands)
	{
		Session session({.link = {.latency = 0.04}, .frame_jitter = 0.0});
		session.run_until(60.0);

		EXPECT_EQ(session.misses_between(5.0, 60.0), 0u);
		EXPECT_NEAR(session.mean_scale(20.0, 60.0), 1.0, 0.001);
		EXPECT_NEAR(session.queue().timing().mean, session.dilation().aim(), 0.75f);

		// Ahead by the trip to the server (2.4 ticks) plus the buffer; the lead is read between ticks.
		EXPECT_NEAR(static_cast<f32>(session.lead()), session.expected_lead(), 1.5f);
		EXPECT_TRUE(session.every_press_landed_once());
	}

	TEST(SessionClock, ATypicalLinkRarelyRunsATickOnAStandIn)
	{
		Session session({.link = {.latency = 0.04, .jitter = 0.005, .loss = 0.01f}});
		session.run_until(120.0);

		const u32 ticks = session.ticks_between(10.0, 120.0);
		EXPECT_LE(session.misses_between(10.0, 120.0), ticks / 1000) << "under 0.1%";
		EXPECT_NEAR(session.mean_scale(30.0, 120.0), 1.0, 0.005);
		EXPECT_TRUE(session.every_press_landed_once()) << "late presses land a tick late, never twice";
	}

	TEST(SessionClock, AHarshLinkEarnsADeeperBuffer)
	{
		Session clean({.link = {.latency = 0.05}});
		Session harsh({.link = {.latency = 0.05, .jitter = 0.02, .loss = 0.05f}});
		clean.run_until(60.0);
		harsh.run_until(60.0);

		EXPECT_GT(harsh.dilation().aim(), clean.dilation().aim() + 0.5f);
		EXPECT_GT(harsh.lead(), clean.lead());

		const u32 ticks = harsh.ticks_between(10.0, 60.0);
		EXPECT_LE(harsh.misses_between(10.0, 60.0), ticks / 100) << "under 1% at 5% loss";
		EXPECT_TRUE(harsh.every_press_landed_once());
	}

	TEST(SessionClock, ALatencyRiseIsCaughtUpWithinASecondOrSo)
	{
		Session session({.link = {.latency = 0.03, .jitter = 0.005}});
		session.run_until(20.0);
		EXPECT_EQ(session.misses_between(5.0, 20.0), 0u);

		session.set_link({.latency = 0.09, .jitter = 0.005}); // 120 ms more round trip
		session.run_until(40.0);

		EXPECT_GT(session.misses_between(20.0, 22.0), 0u) << "the rise itself is felt";
		EXPECT_EQ(session.misses_between(22.5, 40.0), 0u);
		EXPECT_GE(session.jumps(), 1u);
	}

	TEST(SessionClock, ALatencyDropIsReclaimedWithoutLateCommands)
	{
		Session session({.link = {.latency = 0.09, .jitter = 0.005}});
		session.run_until(20.0);
		const Tick before = session.lead();

		session.set_link({.latency = 0.03, .jitter = 0.005});
		session.run_until(40.0);

		EXPECT_EQ(session.misses_between(20.0, 40.0), 0u) << "running too far ahead costs latency, not commands";
		EXPECT_LE(session.lead() + 3, before) << "the 60 ms no longer needed is given back";
		EXPECT_NEAR(static_cast<f32>(session.lead()), session.expected_lead(), 1.5f);
	}

	TEST(SessionClock, AFrameHitchJumpsTheClockForwardAndRecovers)
	{
		Session session({.link = {.latency = 0.04, .jitter = 0.005}});
		session.run_until(20.0);

		const Tick hitch = session.server_tick();
		session.hitch(0.4); // one frame of 400 ms: the TickClock drops most of it
		session.run_until(40.0);

		EXPECT_GE(session.jumps(), 1u);
		EXPECT_EQ(session.misses_between(22.0, 40.0), 0u);
		EXPECT_TRUE(session.every_press_landed_once(hitch - 10, hitch + 120));
		EXPECT_TRUE(session.no_press_invented());
	}

	TEST(SessionClock, AClientThatStartsFarAheadJumpsBack)
	{
		Session session({.link = {.latency = 0.04}, .start_error = 45});
		session.run_until(30.0);

		EXPECT_GE(session.jumps(), 1u);
		EXPECT_NEAR(static_cast<f32>(session.lead()), session.expected_lead(), 1.5f);
		EXPECT_EQ(session.misses_between(5.0, 30.0), 0u);
	}

	TEST(SessionClock, AClientThatStartsBehindJumpsForward)
	{
		Session session({.link = {.latency = 0.04}, .start_error = -12});
		session.run_until(30.0);

		EXPECT_GE(session.jumps(), 1u);
		EXPECT_EQ(session.misses_between(5.0, 30.0), 0u);
	}
}
