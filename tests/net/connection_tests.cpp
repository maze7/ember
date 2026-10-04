#include <ember/net/connection.h>
#include <ember/net/loopback.h>

#include <gtest/gtest.h>

#include <array>
#include <deque>
#include <random>
#include <unordered_set>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::net;

	constexpr f64 STEP = 1.0 / 60.0;

	// The payload of a test packet: its send index, a number that never wraps.
	struct Index
	{
		u64 value = 0;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_uint64(stream, value);
			return true;
		}
	};

	/**
	 * One end of a test link. Every packet carries its send index, so the test can check each notice
	 * against what the other end actually applied.
	 */
	struct End
	{
		Connection connection;
		LoopbackTransport transport;
		PeerId peer = NO_PEER;

		std::deque<std::pair<Sequence, u64>> outstanding; // sent, notice pending: sequence and index
		std::unordered_set<u64> applied;				  // indices of the other end's packets applied here
		u64 next_index = 0;

		u32 reject_every = 0; // refuse to apply every nth packet received, as a failed decode would

		u64 delivered = 0;
		u64 lost	  = 0;

		explicit End(LoopbackNetwork& network) : transport(network) {}

		void send(f64 now)
		{
			PacketBuffer buffer;
			serialize::WriteStream stream = packet_writer(buffer);

			const Sequence sequence = connection.write_header(stream, now);
			ASSERT_TRUE(write(stream, Index{next_index}));
			stream.Flush();

			const Span<const u8> bytes = written(buffer, stream);
			outstanding.emplace_back(sequence, next_index++);
			transport.send(peer, bytes, Delivery::Unreliable, now);
			connection.count_sent(static_cast<u32>(bytes.size()), now);
		}

		void receive(f64 now)
		{
			TransportEvent event;
			while (transport.poll(now, event))
			{
				if (event.kind != TransportEventKind::Received)
					continue;

				PacketBuffer buffer;
				serialize::ReadStream stream;
				ASSERT_TRUE(packet_reader(stream, buffer, event.data));

				Sequence sequence = 0;
				if (connection.read_header(stream, sequence, event.time) != Connection::Arrival::Fresh)
					continue;

				Index index;
				ASSERT_TRUE(read(stream, index));

				if (reject_every != 0 && index.value % reject_every == 0)
					continue;

				applied.insert(index.value);
				connection.acknowledge(sequence, static_cast<u32>(event.data.size()), event.time);
			}
		}

		// Every notice must be for the oldest packet still outstanding, and say delivered exactly
		// when the other end applied it.
		void check_notices(const End& other)
		{
			PacketNotice notice;
			while (connection.take_notice(notice))
			{
				ASSERT_FALSE(outstanding.empty()) << "a notice for a packet never sent";

				const auto [sequence, index] = outstanding.front();
				outstanding.pop_front();

				ASSERT_EQ(notice.sequence, sequence) << "notices come oldest first, one per packet";
				ASSERT_EQ(notice.delivered, other.applied.contains(index)) << "packet " << index;

				(notice.delivered ? delivered : lost) += 1;
			}
		}
	};

	struct TestLink
	{
		LoopbackNetwork network;
		End a{network};
		End b{network};

		explicit TestLink(const LinkConditions& conditions, u64 seed = 1) : network(seed)
		{
			a.transport.set_conditions(conditions);
			b.transport.set_conditions(conditions);

			EXPECT_TRUE(b.transport.listen("b"));
			a.peer = a.transport.connect("b", 0.0).value();

			TransportEvent event;
			EXPECT_TRUE(b.transport.poll(100.0, event));
			b.peer = event.peer;
			EXPECT_TRUE(a.transport.poll(100.0, event));
		}

		/** Steps both ends; b sends only every b_every steps. */
		void run(u32 steps, u32 b_every = 1, f64 start = 1.0)
		{
			for (u32 step = 0; step < steps; ++step)
			{
				const f64 now = start + step * STEP;

				a.send(now);
				if (step % b_every == 0)
					b.send(now);

				a.receive(now);
				b.receive(now);

				a.check_notices(b);
				b.check_notices(a);

				if (::testing::Test::HasFatalFailure())
					return;
			}
		}
	};

	// A header put together by hand, the way a peer that lies would write one.
	struct RawHeader
	{
		u32 sequence  = 0;
		bool has_ack  = false;
		u32 ack		  = 0;
		u32 ack_bits  = 0;
		u32 ack_delay = 0;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, sequence, 16);
			serialize_bool(stream, has_ack);
			serialize_bits(stream, ack, 16);
			serialize_bits(stream, ack_bits, 32);
			serialize_bits(stream, ack_delay, 8);
			return true;
		}
	};

	[[nodiscard]] Connection::Arrival arrive(Connection& receiver, Span<const u8> bytes, Sequence& sequence,
											 f64 time = 1.0)
	{
		PacketBuffer buffer;
		serialize::ReadStream stream;
		if (!packet_reader(stream, buffer, bytes))
			return Connection::Arrival::Malformed;

		return receiver.read_header(stream, sequence, time);
	}

	TEST(Connection, EveryPacketGetsOneNoticeInOrderMatchingWhatThePeerApplied)
	{
		TestLink link({.latency = 0.05, .jitter = 0.02, .loss = 0.2f, .duplicate = 0.05f});
		link.run(3000);

		EXPECT_GT(link.a.delivered, 2000u);
		EXPECT_GT(link.a.lost, 400u);
		EXPECT_LT(link.a.outstanding.size(), 16u) << "only the last round trip is still in flight";
	}

	TEST(Connection, APacketThePeerRefusedToApplyIsLost)
	{
		TestLink link({.latency = 0.03});
		link.b.reject_every = 5;
		link.run(600);

		// No loss on the wire: every lost notice is a refusal.
		EXPECT_NEAR(static_cast<f64>(link.a.lost) / (link.a.lost + link.a.delivered), 0.2, 0.01);
		EXPECT_EQ(link.b.lost, 0u);
	}

	TEST(Connection, ReorderedPacketsAreDroppedAndCountedLost)
	{
		TestLink link({.latency = 0.03, .jitter = 0.03, .reorder = true});
		link.run(2000);

		EXPECT_GT(link.b.connection.stats().packets_stale, 50u);
		EXPECT_EQ(link.a.lost, link.b.connection.stats().packets_stale)
			<< "with no loss on the wire, only the overtaken packets are lost";
	}

	TEST(Connection, SequenceNumbersWrapWithoutLosingTrack)
	{
		TestLink link({.latency = 0.02, .loss = 0.05f});
		link.run(70000);

		EXPECT_GT(link.a.connection.stats().packets_sent, 65536u);
		EXPECT_GT(link.a.delivered, 60000u);
	}

	TEST(Connection, TheRoundTripMatchesTheLink)
	{
		TestLink link({.latency = 0.04});
		link.run(240);

		EXPECT_NEAR(link.a.connection.stats().rtt, 0.08, 0.0005);
		EXPECT_NEAR(link.a.connection.stats().rtt_min, 0.08, 0.0005);
		EXPECT_NEAR(link.a.connection.stats().rtt_variance, 0.0, 0.0005);
	}

	TEST(Connection, AckDelayKeepsASlowPeerFromInflatingTheRoundTrip)
	{
		// b answers every third step, so a's newest packet waits up to two steps for its ack.
		TestLink link({.latency = 0.04});
		link.run(600, 3);

		EXPECT_NEAR(link.a.connection.stats().rtt, 0.08, 0.0005);
	}

	TEST(Connection, LossIsMeasured)
	{
		TestLink link({.latency = 0.02, .loss = 0.1f}, 99);
		link.run(5000);

		const ConnectionStats& stats = link.a.connection.stats();
		EXPECT_NEAR(static_cast<f64>(stats.packets_lost) / (stats.packets_lost + stats.packets_delivered), 0.1, 0.015);
		EXPECT_NEAR(stats.loss, 0.1, 0.07);
		EXPECT_GT(stats.send_rate, 0.0f);
		EXPECT_GT(stats.receive_rate, 0.0f);
	}

	TEST(Connection, TheReceiverMeasuresTheLossItSees)
	{
		TestLink link({.latency = 0.02, .loss = 0.1f}, 7);
		link.run(5000);

		// What one end lost is what the other never applied: the same packets, seen from either side, but
		// for the last round trip's, which the receiver knows of before the acks reach the sender.
		for (const auto& [sender, receiver] : {std::pair{&link.a, &link.b}, std::pair{&link.b, &link.a}})
		{
			const ConnectionStats& sent		= sender->connection.stats();
			const ConnectionStats& received = receiver->connection.stats();

			EXPECT_GE(received.packets_skipped, sent.packets_lost);
			EXPECT_LE(received.packets_skipped, sent.packets_lost + 8);
			EXPECT_NEAR(received.receive_loss, sent.loss, 0.03);
			EXPECT_NEAR(static_cast<f64>(received.packets_skipped) /
							static_cast<f64>(received.packets_skipped + received.packets_received),
						0.1, 0.015);
		}
	}

	TEST(Connection, PacketsNeverAppliedAreSkippedAndBytesAreCounted)
	{
		Connection sender;
		Connection receiver;

		std::array<PacketBuffer, 6> packets{};
		std::array<u32, 6> sizes{};
		u64 sent = 0;
		for (u32 i = 0; i < 6; ++i)
		{
			serialize::WriteStream stream = packet_writer(packets[i]);
			EXPECT_EQ(sender.write_header(stream, 0.0), i);
			stream.Flush();
			sizes[i] = static_cast<u32>(stream.GetBytesProcessed());
			sender.count_sent(sizes[i], 0.0);
			sent += sizes[i];
		}

		const auto deliver = [&](u32 i)
		{
			Sequence sequence = 0;
			const Connection::Arrival arrival =
				arrive(receiver, Span<const u8>(packets[i].bytes.data(), sizes[i]), sequence);

			if (arrival == Connection::Arrival::Fresh)
				receiver.acknowledge(sequence, sizes[i], 1.0);

			return arrival;
		};

		EXPECT_EQ(deliver(0), Connection::Arrival::Fresh);
		EXPECT_EQ(deliver(2), Connection::Arrival::Fresh);
		EXPECT_EQ(deliver(1), Connection::Arrival::Stale) << "passed over, then late";
		EXPECT_EQ(deliver(5), Connection::Arrival::Fresh);

		const ConnectionStats& stats = receiver.stats();
		EXPECT_EQ(stats.packets_received, 3u);
		EXPECT_EQ(stats.packets_skipped, 3u) << "1, 3 and 4: never applied, whether they came late or not at all";
		EXPECT_EQ(stats.packets_stale, 1u);
		EXPECT_GT(stats.receive_loss, 0.0f);
		EXPECT_LT(stats.receive_loss, 0.1f) << "three in six, smoothed over a hundred";
		EXPECT_EQ(stats.bytes_received, u64{sizes[0]} + sizes[2] + sizes[5]) << "applied packets only";
		EXPECT_EQ(sender.stats().bytes_sent, sent);
	}

	TEST(Connection, LateAndRepeatedPacketsAreStale)
	{
		Connection sender;
		Connection receiver;

		std::array<PacketBuffer, 3> packets{};
		std::array<u32, 3> sizes{};
		for (u32 i = 0; i < 3; ++i)
		{
			serialize::WriteStream stream = packet_writer(packets[i]);
			EXPECT_EQ(sender.write_header(stream, 0.0), i);
			stream.Flush();
			sizes[i] = static_cast<u32>(stream.GetBytesProcessed());
		}

		const auto deliver = [&](u32 i)
		{
			Sequence sequence = 0;
			const Connection::Arrival arrival =
				arrive(receiver, Span<const u8>(packets[i].bytes.data(), sizes[i]), sequence);

			if (arrival == Connection::Arrival::Fresh)
			{
				EXPECT_EQ(sequence, i);
				receiver.acknowledge(sequence, sizes[i], 1.0);
			}

			return arrival;
		};

		EXPECT_EQ(deliver(1), Connection::Arrival::Fresh);
		EXPECT_EQ(deliver(0), Connection::Arrival::Stale) << "overtaken";
		EXPECT_EQ(deliver(1), Connection::Arrival::Stale) << "repeated";
		EXPECT_EQ(deliver(2), Connection::Arrival::Fresh);
		EXPECT_EQ(receiver.stats().packets_stale, 2u);
		EXPECT_EQ(receiver.stats().packets_received, 2u);
	}

	TEST(Connection, AnAckForAPacketNeverSentIsMalformed)
	{
		Connection receiver;

		PacketBuffer buffer;
		serialize::WriteStream stream = packet_writer(buffer);
		ASSERT_TRUE(write(stream, RawHeader{.sequence = 0, .has_ack = true, .ack = 5})); // the receiver sent nothing
		stream.Flush();

		Sequence sequence = 0;
		EXPECT_EQ(arrive(receiver, written(buffer, stream), sequence), Connection::Arrival::Malformed);

		const std::array<u8, 3> truncated = {0x01, 0x00, 0x01};
		EXPECT_EQ(arrive(receiver, truncated, sequence), Connection::Arrival::Malformed);
		EXPECT_EQ(receiver.stats().packets_malformed, 2u);
	}

	TEST(Connection, TheHeaderLayoutIsFixed)
	{
		// Peers of different builds, and replays, read headers written by others: pin the bits.
		Connection sender;
		sender.acknowledge(5, 10, 1.0); // as if packet 5 had arrived at t = 1

		PacketBuffer buffer;
		serialize::WriteStream stream = packet_writer(buffer);
		EXPECT_EQ(sender.write_header(stream, 1.0005), 0); // half a millisecond later
		stream.Flush();

		// sequence 0 | has_ack | ack 5 | ack_bits 0 | ack_delay 1, least significant bit first.
		const std::vector<u8> expected = {0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00};
		const Span<const u8> bytes	   = written(buffer, stream);

		EXPECT_EQ(std::vector<u8>(bytes.begin(), bytes.end()), expected);
		EXPECT_EQ(stream.GetBitsProcessed(), Connection::HEADER_BITS);
	}

	TEST(Connection, AFullWindowGivesUpItsOldestPackets)
	{
		Connection sender;

		for (u32 i = 0; i < Connection::WINDOW + 10; ++i)
		{
			PacketBuffer buffer;
			serialize::WriteStream stream = packet_writer(buffer);
			(void)sender.write_header(stream, i * STEP);
		}

		EXPECT_EQ(sender.in_flight(), Connection::WINDOW);

		PacketNotice notice;
		for (u32 i = 0; i < 10; ++i)
		{
			ASSERT_TRUE(sender.take_notice(notice));
			EXPECT_EQ(notice.sequence, i);
			EXPECT_FALSE(notice.delivered);
		}

		EXPECT_FALSE(sender.take_notice(notice));
	}

	TEST(Connection, SilenceTimesOut)
	{
		Connection connection({.timeout = 2.0}, 10.0);

		EXPECT_FALSE(connection.timed_out(11.9));
		EXPECT_TRUE(connection.timed_out(12.1));

		connection.acknowledge(0, 10, 12.0);
		EXPECT_FALSE(connection.timed_out(13.9));
		EXPECT_TRUE(connection.timed_out(14.1));
	}

	TEST(Connection, GarbageNeverBreaksTheBookkeeping)
	{
		Connection connection;

		for (u32 i = 0; i < 40; ++i)
		{
			PacketBuffer buffer;
			serialize::WriteStream stream = packet_writer(buffer);
			(void)connection.write_header(stream, 0.0);
		}

		std::mt19937 random(42);
		u64 fresh = 0;

		for (u32 round = 0; round < 20000; ++round)
		{
			std::vector<u8> junk(random() % 12);
			for (u8& byte : junk)
				byte = static_cast<u8>(random());

			Sequence sequence = 0;
			if (arrive(connection, junk, sequence) == Connection::Arrival::Fresh)
			{
				++fresh;
				connection.acknowledge(sequence, static_cast<u32>(junk.size()), 1.0);
			}
		}

		const ConnectionStats& stats = connection.stats();
		EXPECT_EQ(stats.packets_received, fresh);
		EXPECT_EQ(stats.packets_received + stats.packets_stale + stats.packets_malformed, 20000u);
		EXPECT_EQ(stats.packets_delivered + stats.packets_lost + connection.in_flight(), 40u)
			<< "lies can settle packets early, never twice or out of thin air";
	}
}
