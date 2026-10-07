#include <ember/net/loopback.h>

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::net;

	struct Polled
	{
		TransportEventKind kind = TransportEventKind::Received;
		PeerId peer				= NO_PEER;
		f64 time				= 0.0;
		std::vector<u8> data;
		Delivery delivery		= Delivery::Unreliable;
		DisconnectReason reason = DisconnectReason::None;
	};

	[[nodiscard]] std::vector<Polled> drain(Transport& transport, f64 now)
	{
		std::vector<Polled> events;
		TransportEvent event;

		while (transport.poll(now, event))
			events.push_back({event.kind,
							  event.peer,
							  event.time,
							  {event.data.begin(), event.data.end()},
							  event.delivery,
							  event.reason});

		return events;
	}

	[[nodiscard]] std::array<u8, 4> datagram(u32 value)
	{
		return {static_cast<u8>(value), static_cast<u8>(value >> 8), static_cast<u8>(value >> 16),
				static_cast<u8>(value >> 24)};
	}

	[[nodiscard]] u32 value_of(const std::vector<u8>& data)
	{
		return data[0] | (data[1] << 8) | (data[2] << 16) | (static_cast<u32>(data[3]) << 24);
	}

	// A server and a client on one network, both ends with the same conditions.
	struct Pair
	{
		LoopbackNetwork network;
		LoopbackTransport server{network};
		LoopbackTransport client{network};
		PeerId to_server = NO_PEER;
		PeerId to_client = NO_PEER;

		explicit Pair(const LinkConditions& conditions = {}, u64 seed = 1) : network(seed)
		{
			server.set_conditions(conditions);
			client.set_conditions(conditions);

			EXPECT_TRUE(server.listen("server"));
			to_server = client.connect("server", 0.0).value();

			const std::vector<Polled> accepted = drain(server, 10.0 * conditions.latency + 1.0);
			EXPECT_EQ(accepted.size(), 1u);
			to_client = accepted.empty() ? NO_PEER : accepted[0].peer;

			const std::vector<Polled> connected = drain(client, 10.0 * conditions.latency + 1.0);
			EXPECT_EQ(connected.size(), 1u);
		}
	};

	TEST(Loopback, ConnectingReachesTheServerOneWayInAndTheClientARoundTripAfter)
	{
		LoopbackNetwork network;
		LoopbackTransport server(network);
		LoopbackTransport client(network);

		server.set_conditions({.latency = 0.05});
		client.set_conditions({.latency = 0.03});

		ASSERT_TRUE(server.listen("arena"));
		const PeerId to_server = client.connect("arena", 1.0).value();
		EXPECT_NE(to_server, NO_PEER);

		EXPECT_TRUE(drain(server, 1.029).empty());
		const std::vector<Polled> accepted = drain(server, 1.03);
		ASSERT_EQ(accepted.size(), 1u);
		EXPECT_EQ(accepted[0].kind, TransportEventKind::Connected);

		EXPECT_TRUE(drain(client, 1.079).empty());
		const std::vector<Polled> connected = drain(client, 1.08);
		ASSERT_EQ(connected.size(), 1u);
		EXPECT_EQ(connected[0].kind, TransportEventKind::Connected);
		EXPECT_EQ(connected[0].peer, to_server);
	}

	TEST(Loopback, AddressesAreClaimedOnceAndMustExist)
	{
		LoopbackNetwork network;
		LoopbackTransport first(network);
		LoopbackTransport second(network);

		EXPECT_EQ(first.listen("").error(), TransportError::BadAddress);
		ASSERT_TRUE(first.listen("a"));
		EXPECT_EQ(first.listen("b").error(), TransportError::AlreadyListening);
		EXPECT_EQ(second.listen("a").error(), TransportError::AddressInUse);
		EXPECT_EQ(second.connect("nowhere", 0.0).error(), TransportError::Unreachable);
	}

	TEST(Loopback, DatagramsArriveAfterTheLatencyInTheOrderSent)
	{
		Pair pair({.latency = 0.02, .jitter = 0.05});

		for (u32 i = 0; i < 100; ++i)
			pair.client.send(pair.to_server, datagram(i), Delivery::Unreliable, 10.0 + i * 0.001);

		EXPECT_TRUE(drain(pair.server, 10.0199).empty());

		const std::vector<Polled> received = drain(pair.server, 11.0);
		ASSERT_EQ(received.size(), 100u);

		for (u32 i = 0; i < 100; ++i)
		{
			EXPECT_EQ(received[i].peer, pair.to_client);
			EXPECT_EQ(value_of(received[i].data), i) << "jitter without reorder keeps the order";
			EXPECT_GE(received[i].time, 10.0 + i * 0.001 + 0.02);
		}
	}

	TEST(Loopback, ReorderLetsJitterSwapDatagrams)
	{
		Pair pair({.latency = 0.02, .jitter = 0.05, .reorder = true});

		for (u32 i = 0; i < 200; ++i)
			pair.client.send(pair.to_server, datagram(i), Delivery::Unreliable, 10.0 + i * 0.001);

		const std::vector<Polled> received = drain(pair.server, 11.0);
		ASSERT_EQ(received.size(), 200u);

		u32 swapped = 0;
		for (u32 i = 1; i < received.size(); ++i)
			swapped += value_of(received[i].data) < value_of(received[i - 1].data) ? 1 : 0;

		EXPECT_GT(swapped, 10u);
	}

	TEST(Loopback, LossAndDuplicationHitTheirRates)
	{
		Pair pair({.loss = 0.2f, .duplicate = 0.1f});

		constexpr u32 SENT = 20000;
		for (u32 i = 0; i < SENT; ++i)
			pair.client.send(pair.to_server, datagram(i), Delivery::Unreliable, 10.0);

		std::vector<u32> copies(SENT, 0);
		for (const Polled& event : drain(pair.server, 11.0))
			++copies[value_of(event.data)];

		u32 lost = 0, doubled = 0;
		for (const u32 count : copies)
		{
			lost += count == 0 ? 1 : 0;
			doubled += count == 2 ? 1 : 0;
		}

		// A lost datagram can still have its duplicate arrive, so "none" is loss without a copy.
		EXPECT_NEAR(static_cast<f64>(lost) / SENT, 0.2 * 0.9, 0.015);
		EXPECT_NEAR(static_cast<f64>(doubled) / SENT, 0.8 * 0.1, 0.015);
	}

	TEST(Loopback, TheSameSeedReplaysTheSameWeather)
	{
		const auto run = [](u64 seed)
		{
			Pair pair({.latency = 0.01, .jitter = 0.02, .loss = 0.3f, .reorder = true}, seed);

			for (u32 i = 0; i < 500; ++i)
				pair.client.send(pair.to_server, datagram(i), Delivery::Unreliable, 5.0 + i * 0.002);

			std::vector<u32> order;
			for (const Polled& event : drain(pair.server, 10.0))
				order.push_back(value_of(event.data));

			return order;
		};

		EXPECT_EQ(run(7), run(7));
		EXPECT_NE(run(7), run(8));
	}

	TEST(Loopback, DisconnectingReachesThePeerBehindItsDatagramsAndClosesTheLink)
	{
		Pair pair({.latency = 0.01});

		pair.client.send(pair.to_server, datagram(1), Delivery::Unreliable, 5.0);
		pair.client.disconnect(pair.to_server, DisconnectReason::Requested, 5.0);
		pair.client.send(pair.to_server, datagram(2), Delivery::Unreliable, 5.0); // the link is closed: dropped

		EXPECT_EQ(pair.client.connection_count(), 0u);

		const std::vector<Polled> events = drain(pair.server, 6.0);
		ASSERT_EQ(events.size(), 2u);
		EXPECT_EQ(events[0].kind, TransportEventKind::Received);
		EXPECT_EQ(events[1].kind, TransportEventKind::Disconnected);
		EXPECT_EQ(events[1].reason, DisconnectReason::Requested);
		EXPECT_EQ(pair.server.connection_count(), 0u);

		// Nothing more crosses a closed link, either way.
		pair.server.send(pair.to_client, datagram(3), Delivery::Unreliable, 6.0);
		EXPECT_TRUE(drain(pair.client, 7.0).empty());
	}

	TEST(Loopback, DatagramsToAClosedLinkAreDroppedOnArrival)
	{
		Pair pair({.latency = 0.05});

		pair.client.send(pair.to_server, datagram(1), Delivery::Unreliable, 5.0);
		pair.server.disconnect(pair.to_client, DisconnectReason::Kicked, 5.0);

		EXPECT_TRUE(drain(pair.server, 6.0).empty()) << "the server closed it; nothing arrives for it";

		const std::vector<Polled> events = drain(pair.client, 6.0);
		ASSERT_EQ(events.size(), 1u);
		EXPECT_EQ(events[0].kind, TransportEventKind::Disconnected);
		EXPECT_EQ(events[0].reason, DisconnectReason::Kicked);
	}

	TEST(Loopback, DestroyingATransportDisconnectsItsPeers)
	{
		LoopbackNetwork network;
		LoopbackTransport server(network);
		ASSERT_TRUE(server.listen("server"));

		{
			LoopbackTransport client(network);
			ASSERT_TRUE(client.connect("server", 0.0));
			client.send(1, datagram(9), Delivery::Unreliable, 0.0);
		}

		const std::vector<Polled> events = drain(server, 1.0);
		ASSERT_EQ(events.size(), 2u);
		EXPECT_EQ(events[0].kind, TransportEventKind::Connected);
		EXPECT_EQ(events[1].kind, TransportEventKind::Disconnected);
		EXPECT_EQ(events[1].reason, DisconnectReason::Shutdown);
		EXPECT_EQ(network.in_flight(), 0u) << "the dead client's datagram dies with its link";
	}

	TEST(Loopback, ReliableSendsArriveOnceAndInOrderWhateverTheWeather)
	{
		Pair pair({.latency = 0.02, .jitter = 0.05, .loss = 0.5f, .duplicate = 0.5f, .reorder = true});

		for (u32 i = 0; i < 300; ++i)
			pair.client.send(pair.to_server, datagram(i), Delivery::Reliable, 10.0 + i * 0.001);

		const std::vector<Polled> received = drain(pair.server, 11.0);
		ASSERT_EQ(received.size(), 300u);

		for (u32 i = 0; i < received.size(); ++i)
		{
			EXPECT_EQ(value_of(received[i].data), i);
			EXPECT_EQ(received[i].delivery, Delivery::Reliable);
		}
	}

	TEST(Loopback, StatsReportTheConfiguredWeather)
	{
		LoopbackNetwork network;
		LoopbackTransport server(network);
		LoopbackTransport client(network);

		server.set_conditions({.latency = 0.05, .loss = 0.1f});
		client.set_conditions({.latency = 0.03});

		ASSERT_TRUE(server.listen("server"));
		const PeerId to_server = client.connect("server", 0.0).value();

		const PeerStats stats = client.stats(to_server);
		EXPECT_FLOAT_EQ(stats.ping, 0.08f);
		EXPECT_FLOAT_EQ(stats.quality_local, 0.9f) << "the server's sends reach the client nine times in ten";
		EXPECT_FLOAT_EQ(stats.quality_remote, 1.0f);
		EXPECT_EQ(client.stats(to_server + 1).ping, 0.0f) << "an unknown peer has no path";
	}

	TEST(Loopback, BulkSendsArriveWholeAndInOrderOnAStreamOfTheirOwn)
	{
		// Jitter and reordering: unreliable datagrams may overtake each other, bulk sends never do.
		Pair pair({.latency = 0.05, .jitter = 0.04, .reorder = true}, 7);

		std::vector<std::vector<u8>> sent;
		for (u32 i = 0; i < 3; ++i)
		{
			std::vector<u8> bytes(i == 1 ? MAX_BULK_BYTES : 1500 + 700 * i);
			for (size_t b = 0; b < bytes.size(); ++b)
				bytes[b] = static_cast<u8>(b * 31 + i);

			EXPECT_TRUE(pair.client.send(pair.to_server, bytes, Delivery::Bulk, 0.0));
			EXPECT_TRUE(pair.client.send(pair.to_server, datagram(i), Delivery::Unreliable, 0.0));
			sent.push_back(std::move(bytes));
		}

		const std::vector<Polled> events = drain(pair.server, 1.0);

		std::vector<const Polled*> bulk;
		for (const Polled& event : events)
			if (event.delivery == Delivery::Bulk)
				bulk.push_back(&event);

		ASSERT_EQ(bulk.size(), 3u);
		for (u32 i = 0; i < 3; ++i)
			EXPECT_EQ(bulk[i]->data, sent[i]);

		EXPECT_LE(bulk[0]->time, bulk[1]->time);
		EXPECT_LE(bulk[1]->time, bulk[2]->time);
	}

	TEST(Loopback, SendSaysWhetherTheTransportTookIt)
	{
		Pair pair;

		const std::vector<u8> largest(MAX_BULK_BYTES, 0xAB);
		EXPECT_TRUE(pair.server.send(pair.to_client, largest, Delivery::Bulk, 0.0));
		EXPECT_FALSE(pair.server.send(999, largest, Delivery::Bulk, 0.0)) << "no such peer";

		const std::vector<Polled> events = drain(pair.client, 1.0);
		ASSERT_EQ(events.size(), 1u);
		EXPECT_EQ(events[0].delivery, Delivery::Bulk);
		EXPECT_EQ(events[0].data, largest);

		// A closed link takes nothing more.
		pair.client.disconnect(pair.to_server, DisconnectReason::Requested, 1.0);
		EXPECT_FALSE(pair.client.send(pair.to_server, largest, Delivery::Bulk, 1.0));
	}
}
