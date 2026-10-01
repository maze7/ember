#include <ember/net/valve_transport.h>

#include <gtest/gtest.h>
#include <steam/isteamnetworkingsockets.h>

#if EMBER_USE_STEAM
	#include <steam/steam_api.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::net;

	// The sockets run on real time, the steady clock in seconds.
	[[nodiscard]] f64 wall_clock()
	{
		return std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	struct Polled
	{
		TransportEventKind kind = TransportEventKind::Received;
		PeerId peer				= NO_PEER;
		f64 time				= 0.0;
		std::vector<u8> data;
		Delivery delivery		= Delivery::Unreliable;
		DisconnectReason reason = DisconnectReason::None;
	};

	struct End
	{
		ValveTransport transport;
		std::vector<Polled> events;

		void drain()
		{
			TransportEvent event;
			while (transport.poll(wall_clock(), event))
			{
				events.push_back({event.kind,
								  event.peer,
								  event.time,
								  {event.data.begin(), event.data.end()},
								  event.delivery,
								  event.reason});
			}
		}

		[[nodiscard]] u32 count(TransportEventKind kind) const
		{
			return static_cast<u32>(std::count_if(events.begin(), events.end(),
												  [kind](const Polled& event) { return event.kind == kind; }));
		}

		[[nodiscard]] std::vector<u32> received(Delivery delivery) const
		{
			std::vector<u32> values;
			for (const Polled& event : events)
			{
				if (event.kind == TransportEventKind::Received && event.delivery == delivery)
					values.push_back(event.data[0] | (event.data[1] << 8) | (event.data[2] << 16) |
									 (static_cast<u32>(event.data[3]) << 24));
			}
			return values;
		}
	};

	[[nodiscard]] std::array<u8, 4> datagram(u32 value)
	{
		return {static_cast<u8>(value), static_cast<u8>(value >> 8), static_cast<u8>(value >> 16),
				static_cast<u8>(value >> 24)};
	}

	// Poll both ends until done, or give up at the deadline.
	template <class Done> bool pump(End& a, End& b, Done&& done, f64 seconds = 5.0)
	{
		const f64 deadline = wall_clock() + seconds;

		while (wall_clock() < deadline)
		{
			a.drain();
			b.drain();

			if (done())
				return true;

			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		return false;
	}

	/**
	 * A GameNetworkingSockets build starts the library for these tests. A Steam build signs in as
	 * whoever is logged in to Steam, and friends see them playing, so its tests run only when asked:
	 * with Steam running and SteamAppId=480 (Spacewar, Valve's test app) in the environment.
	 */
	class Valve : public ::testing::Test
	{
	protected:
		static void SetUpTestSuite()
		{
#if EMBER_USE_STEAM
			if (std::getenv("SteamAppId") == nullptr)
			{
				s_skip = "a Steam build: run with Steam running and SteamAppId=480 to test it";
				return;
			}

			SteamErrMsg message = {};
			if (SteamAPI_InitEx(&message) != k_ESteamAPIInitResult_OK)
			{
				s_skip = std::string("Steam did not start: ") + message;
				return;
			}

			s_steam = true;
#endif
			ASSERT_TRUE(initialize());
			s_ready = true;
		}

		static void TearDownTestSuite()
		{
			if (s_ready)
			{
				simulate({});
				shutdown();
			}

#if EMBER_USE_STEAM
			if (s_steam)
				SteamAPI_Shutdown();
#endif
		}

		void SetUp() override
		{
			if (!s_ready)
				GTEST_SKIP() << s_skip;
		}

		void TearDown() override
		{
			if (s_ready)
				simulate({});
		}

		static inline bool s_ready		 = false;
		static inline std::string s_skip = "networking did not start";
#if EMBER_USE_STEAM
		static inline bool s_steam = false;
#endif
	};

	TEST_F(Valve, StartsOncePerProcess)
	{
		EXPECT_TRUE(is_initialized());
		EXPECT_EQ(initialize().error(), TransportError::AlreadyInitialized);
	}

	TEST_F(Valve, ATransportMadeBeforeTheLibraryStartsRefusesEverything)
	{
		shutdown();

		{
			ValveTransport transport;
			EXPECT_EQ(transport.listen(":0").error(), TransportError::NotInitialized);
			EXPECT_EQ(transport.connect("127.0.0.1:27015", wall_clock()).error(), TransportError::NotInitialized);

			TransportEvent event;
			EXPECT_FALSE(transport.poll(wall_clock(), event));
		}

		ASSERT_TRUE(initialize());
	}

	TEST_F(Valve, TheServerRoleNeedsSteamsGameServer)
	{
		shutdown();

		// This process never called SteamGameServer_Init. GameNetworkingSockets has one interface and
		// ignores the role.
		const Result<void, TransportError> started = initialize({.role = Role::Server});
		if constexpr (STEAM_NETWORKING)
		{
			EXPECT_EQ(started.error(), TransportError::SteamNotRunning);
			EXPECT_FALSE(is_initialized());
		}
		else
		{
			EXPECT_TRUE(started);
			shutdown();
		}

		ASSERT_TRUE(initialize());
	}

	TEST_F(Valve, BadAddressesAreRefused)
	{
		ValveTransport transport;

		EXPECT_EQ(transport.listen("").error(), TransportError::BadAddress);
		EXPECT_EQ(transport.listen("not an address").error(), TransportError::BadAddress);
		EXPECT_EQ(transport.listen(":99999").error(), TransportError::BadAddress);
		EXPECT_EQ(transport.listen(":0").error(), TransportError::BadAddress) << "the library binds only a given port";
		EXPECT_EQ(transport.connect("127.0.0.1", wall_clock()).error(), TransportError::BadAddress) << "no port";
		EXPECT_EQ(transport.connect(":27015", wall_clock()).error(), TransportError::BadAddress) << "no host";
	}

	TEST_F(Valve, SteamAddressesReadTheSameInEveryBuild)
	{
		ValveTransport transport;
		const f64 now = wall_clock();

		// Malformed, whatever the build.
		EXPECT_EQ(transport.listen("steam:").error(), TransportError::BadAddress);
		EXPECT_EQ(transport.listen("steam:port").error(), TransportError::BadAddress);
		EXPECT_EQ(transport.listen("steam:70000").error(), TransportError::BadAddress);
		EXPECT_EQ(transport.connect("steam", now).error(), TransportError::BadAddress) << "whom to reach?";
		EXPECT_EQ(transport.connect("steam:0", now).error(), TransportError::BadAddress) << "no one is Steam ID 0";
		EXPECT_EQ(transport.connect("steam:-5", now).error(), TransportError::BadAddress);
		EXPECT_EQ(transport.connect("steam:76561197960265729:", now).error(), TransportError::BadAddress);
		EXPECT_EQ(transport.connect("steam:76561197960265729:70000", now).error(), TransportError::BadAddress);

		if constexpr (!STEAM_NETWORKING)
		{
			// Well formed, but only Steam reaches anyone by Steam ID.
			EXPECT_EQ(transport.listen("steam").error(), TransportError::Unsupported);
			EXPECT_EQ(transport.listen("steam:1").error(), TransportError::Unsupported);
			EXPECT_EQ(transport.connect("steam:76561197960265729", now).error(), TransportError::Unsupported);
			EXPECT_EQ(transport.connect("steam:76561197960265729:2", now).error(), TransportError::Unsupported);
		}
		else
		{
			// Listening through the relays takes no UDP port.
			ASSERT_TRUE(transport.listen("steam"));
			EXPECT_EQ(transport.listen_port(), 0);
		}
	}

	TEST_F(Valve, ALocalPairConnectsAtOnceAndCarriesBothKinds)
	{
		End server;
		End client;

		const PeerId to_server = client.transport.connect_local(server.transport, false, wall_clock()).value();

		ASSERT_TRUE(pump(server, client,
						 [&]
						 {
							 return server.count(TransportEventKind::Connected) == 1 &&
									client.count(TransportEventKind::Connected) == 1;
						 }));
		EXPECT_EQ(client.events[0].peer, to_server);
		const PeerId to_client = server.events[0].peer;

		const f64 sent = wall_clock();
		for (u32 i = 0; i < 50; ++i)
		{
			client.transport.send(to_server, datagram(i), Delivery::Unreliable, wall_clock());
			client.transport.send(to_server, datagram(1000 + i), Delivery::Reliable, wall_clock());
		}

		ASSERT_TRUE(pump(server, client, [&] { return server.count(TransportEventKind::Received) == 100; }));

		std::vector<u32> reliable	= server.received(Delivery::Reliable);
		std::vector<u32> unreliable = server.received(Delivery::Unreliable);
		ASSERT_EQ(reliable.size(), 50u);
		ASSERT_EQ(unreliable.size(), 50u) << "nothing is lost without the network";

		for (u32 i = 0; i < 50; ++i)
			EXPECT_EQ(reliable[i], 1000 + i) << "reliable messages arrive in order";

		for (const Polled& event : server.events)
		{
			if (event.kind == TransportEventKind::Received)
			{
				EXPECT_EQ(event.peer, to_client);
				EXPECT_GE(event.time, sent - 0.001);
				EXPECT_LE(event.time, wall_clock());
			}
		}

		// And back.
		server.transport.send(to_client, datagram(7), Delivery::Reliable, wall_clock());
		ASSERT_TRUE(pump(server, client, [&] { return client.count(TransportEventKind::Received) == 1; }));
		EXPECT_EQ(client.received(Delivery::Reliable), std::vector<u32>{7});

		// GameNetworkingSockets proves no Steam identity; nobody can for a peer that does not exist.
		if constexpr (!STEAM_NETWORKING)
		{
			EXPECT_EQ(server.transport.steam_id(to_client), 0u);
		}
		EXPECT_EQ(server.transport.steam_id(to_client + 100), 0u);
	}

	TEST_F(Valve, ClientAndServerConnectOverUdp)
	{
		End server;
		End client;

		// The library binds only the port it is given: take the first free one of a few, on every
		// interface.
		u16 port = 0;
		for (u16 candidate = 41'000; candidate < 41'100 && port == 0; ++candidate)
		{
			std::string every_interface = ":";
			every_interface += std::to_string(candidate);

			if (server.transport.listen(every_interface))
				port = candidate;
		}

		ASSERT_NE(port, 0);
		EXPECT_EQ(server.transport.listen_port(), port);
		EXPECT_EQ(server.transport.listen("127.0.0.1:41100").error(), TransportError::AlreadyListening);

		const std::string address = "127.0.0.1:" + std::to_string(port);
		const PeerId to_server	  = client.transport.connect(address, wall_clock()).value();

		ASSERT_TRUE(pump(server, client,
						 [&]
						 {
							 return server.count(TransportEventKind::Connected) == 1 &&
									client.count(TransportEventKind::Connected) == 1;
						 }));

		client.transport.send(to_server, datagram(42), Delivery::Unreliable, wall_clock());
		client.transport.send(to_server, datagram(43), Delivery::Reliable, wall_clock());
		ASSERT_TRUE(pump(server, client, [&] { return server.count(TransportEventKind::Received) == 2; }));

		const PeerStats stats = client.transport.stats(to_server);
		EXPECT_GE(stats.ping, 0.0f);
		EXPECT_GT(stats.send_rate, 0.0f);
		EXPECT_EQ(client.transport.stats(to_server + 100).send_rate, 0.0f) << "an unknown peer has no path";
	}

	TEST_F(Valve, ClosingAConnectionTellsThePeerWhy)
	{
		End server;
		End client;

		const PeerId to_server = client.transport.connect_local(server.transport, false, wall_clock()).value();
		ASSERT_TRUE(pump(server, client, [&] { return server.count(TransportEventKind::Connected) == 1; }));

		client.transport.disconnect(to_server, DisconnectReason::Kicked, wall_clock());
		EXPECT_EQ(client.transport.connection_count(), 0u);

		ASSERT_TRUE(pump(server, client, [&] { return server.count(TransportEventKind::Disconnected) == 1; }));
		EXPECT_EQ(server.events.back().reason, DisconnectReason::Kicked);
		EXPECT_EQ(server.transport.connection_count(), 0u);
		EXPECT_EQ(client.count(TransportEventKind::Disconnected), 0u) << "the end that closed hears nothing more";
	}

	TEST_F(Valve, DestroyingATransportDisconnectsItsPeers)
	{
		End server;

		{
			End client;
			ASSERT_TRUE(client.transport.connect_local(server.transport, false, wall_clock()));
			ASSERT_TRUE(pump(server, client, [&] { return server.count(TransportEventKind::Connected) == 1; }));
		}

		End nobody;
		ASSERT_TRUE(pump(server, nobody, [&] { return server.count(TransportEventKind::Disconnected) == 1; }));
		EXPECT_EQ(server.events.back().reason, DisconnectReason::Shutdown);
	}

	TEST_F(Valve, AMessageLargerThanAPacketIsDropped)
	{
		End server;
		End nobody;

		u16 port = 0;
		for (u16 candidate = 41'200; candidate < 41'300 && port == 0; ++candidate)
		{
			if (server.transport.listen("127.0.0.1:" + std::to_string(candidate)))
				port = candidate;
		}
		ASSERT_NE(port, 0);

		// A peer that is not this engine: the library itself, sending what ValveTransport never would.
		ISteamNetworkingSockets& sockets = *SteamNetworkingSockets();
		SteamNetworkingIPAddr address;
		address.SetIPv4(0x7f000001, port);
		const HSteamNetConnection hostile = sockets.ConnectByIPAddress(address, 0, nullptr);
		ASSERT_NE(hostile, k_HSteamNetConnection_Invalid);

		ASSERT_TRUE(pump(server, nobody,
						 [&]
						 {
							 SteamNetConnectionInfo_t info;
							 return sockets.GetConnectionInfo(hostile, &info) &&
									info.m_eState == k_ESteamNetworkingConnectionState_Connected &&
									server.count(TransportEventKind::Connected) == 1;
						 }));

		const std::vector<u8> huge(MAX_PACKET_BYTES + 1000, 0xab);
		sockets.SendMessageToConnection(hostile, huge.data(), static_cast<uint32>(huge.size()),
										k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
		sockets.SendMessageToConnection(hostile, datagram(5).data(), 4, k_nSteamNetworkingSend_ReliableNoNagle,
										nullptr);

		ASSERT_TRUE(pump(server, nobody, [&] { return server.count(TransportEventKind::Received) >= 1; }));
		pump(server, nobody, [] { return false; }, 0.1);

		EXPECT_EQ(server.received(Delivery::Reliable), std::vector<u32>{5}) << "the oversized message never surfaces";
		sockets.CloseConnection(hostile, 0, nullptr, false);
	}

	TEST_F(Valve, SimulatedLossDropsUnreliableSendsAndNeverReliableOnes)
	{
		simulate({.loss = 0.5f});

		End server;
		End client;

		const PeerId to_server = client.transport.connect_local(server.transport, true, wall_clock()).value();
		ASSERT_TRUE(pump(server, client, [&] { return server.count(TransportEventKind::Connected) == 1; }));

		// A millisecond apart, so each send leaves in a packet of its own: the library packs a burst of
		// sends into a few packets, and the loss, applied per packet, would take them in clumps.
		for (u32 i = 0; i < 200; ++i)
		{
			client.transport.send(to_server, datagram(i), Delivery::Unreliable, wall_clock());
			if (i % 5 == 0)
				client.transport.send(to_server, datagram(1000 + i / 5), Delivery::Reliable, wall_clock());

			pump(server, client, [] { return false; }, 0.001);
		}

		ASSERT_TRUE(pump(server, client, [&] { return server.received(Delivery::Reliable).size() == 40; }, 10.0));
		pump(server, client, [] { return false; }, 0.3); // stragglers

		const std::vector<u32> reliable = server.received(Delivery::Reliable);
		for (u32 i = 0; i < 40; ++i)
			EXPECT_EQ(reliable[i], 1000 + i);

		// Half of 200, give or take five deviations of the coin.
		const size_t unreliable = server.received(Delivery::Unreliable).size();
		EXPECT_GT(unreliable, 65u);
		EXPECT_LT(unreliable, 135u);
	}

	TEST_F(Valve, ArrivalTimesMarkWhenADatagramReachedTheMachine)
	{
		simulate({.latency = 0.05});

		End server;
		End client;

		const PeerId to_server = client.transport.connect_local(server.transport, true, wall_clock()).value();
		ASSERT_TRUE(pump(server, client, [&] { return server.count(TransportEventKind::Connected) == 1; }));

		const f64 sent = wall_clock();
		client.transport.send(to_server, datagram(1), Delivery::Unreliable, sent);

		// Look late on purpose: the arrival must still say when it came, not when it was read.
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		ASSERT_TRUE(pump(server, client, [&] { return server.count(TransportEventKind::Received) == 1; }));

		const f64 travel = server.events.back().time - sent;
		EXPECT_GT(travel, 0.04);
		EXPECT_LT(travel, 0.2);
	}
}
