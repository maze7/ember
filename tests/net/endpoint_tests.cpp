#include <ember/net/client.h>
#include <ember/net/loopback.h>
#include <ember/net/server.h>

#include <gtest/gtest.h>

#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::net;

	constexpr f64 DT = 1.0 / 60.0;

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

	void repeat_command(Input&) {}
	void merge_late_command(Input&, const Input&) {}

	[[nodiscard]] std::vector<u8> pattern(size_t size, u8 seed)
	{
		std::vector<u8> bytes(size);
		for (size_t i = 0; i < size; ++i)
			bytes[i] = static_cast<u8>(seed + i * 7);
		return bytes;
	}

	/** A server and one client over a loopback with no weather, welcomed: what a session starts from. */
	struct Session
	{
		LoopbackNetwork network;
		LoopbackTransport server_end{network};
		LoopbackTransport client_end{network};
		Server server{server_end, {.commands = command_codec<Input>(), .game_protocol = 7, .max_clients = 2}};
		Client client{client_end, {.commands = command_codec<Input>(), .game_protocol = 7}};

		f64 now	  = 0.0;
		Tick tick = 100;
		u8 slot	  = 0;

		std::vector<ServerEventKind> server_events;
		std::vector<ClientEventKind> client_events;
		std::vector<std::vector<u8>> server_bulk; // what the server received on the bulk lane, in order
		std::vector<std::vector<u8>> client_bulk;

		Session()
		{
			EXPECT_TRUE(server.listen("server"));
			EXPECT_TRUE(client.connect("server", now));

			for (u32 i = 0; i < 20 && client.state() != ClientState::Playing; ++i)
				step();

			EXPECT_EQ(client.state(), ClientState::Playing);
		}

		/** One tick: time moves, the server polls and sends its packets, the client polls and sends its command. */
		void step()
		{
			now += DT;

			ServerEvent server_event;
			while (server.poll(tick, now, server_event))
			{
				server_events.push_back(server_event.kind);

				if (server_event.kind == ServerEventKind::Joined)
					slot = server_event.slot;

				if (server_event.kind == ServerEventKind::Bulk)
					server_bulk.emplace_back(server_event.data.begin(), server_event.data.end());
			}

			server.send_packets(now);
			++tick;

			ClientEvent client_event;
			while (client.poll(now, client_event))
			{
				client_events.push_back(client_event.kind);

				if (client_event.kind == ClientEventKind::Bulk)
					client_bulk.emplace_back(client_event.data.begin(), client_event.data.end());
			}

			client.send_command(tick, Input{}, 0.0f, now);
		}
	};

	TEST(Endpoints, BulkTravelsBothWaysWholeAndInOrder)
	{
		Session session;

		const std::vector<u8> up	= pattern(MAX_BULK_BYTES, 3);
		const std::vector<u8> down1 = pattern(5000, 9);
		const std::vector<u8> down2 = pattern(MAX_BULK_BYTES, 11);

		EXPECT_TRUE(session.client.send_bulk(up, session.now));
		EXPECT_TRUE(session.server.send_bulk(session.slot, down1, session.now));
		EXPECT_TRUE(session.server.send_bulk(session.slot, down2, session.now));

		for (u32 i = 0; i < 5; ++i)
			session.step();

		ASSERT_EQ(session.server_bulk.size(), 1u);
		EXPECT_EQ(session.server_bulk[0], up);

		ASSERT_EQ(session.client_bulk.size(), 2u);
		EXPECT_EQ(session.client_bulk[0], down1);
		EXPECT_EQ(session.client_bulk[1], down2);

		// The game's own traffic went on meanwhile: a Joined each side, no Left.
		EXPECT_EQ(std::count(session.server_events.begin(), session.server_events.end(), ServerEventKind::Left), 0);
		EXPECT_EQ(std::count(session.client_events.begin(), session.client_events.end(), ClientEventKind::Left), 0);
	}

	TEST(Endpoints, AClientNotYetPlayingSendsNoBulk)
	{
		LoopbackNetwork network;
		LoopbackTransport client_end{network};
		Client client{client_end, {.commands = command_codec<Input>(), .game_protocol = 7}};

		const std::vector<u8> bytes(100, 1);
		EXPECT_FALSE(client.send_bulk(bytes, 0.0));
	}

	TEST(Endpoints, BulkFromAPeerBeforeItsHelloIsRefusedAsMalformed)
	{
		LoopbackNetwork network;
		LoopbackTransport server_end{network};
		LoopbackTransport rogue{network};
		Server server{server_end, {.commands = command_codec<Input>(), .game_protocol = 7}};

		EXPECT_TRUE(server.listen("server"));
		const PeerId to_server = rogue.connect("server", 0.0).value();

		f64 now = DT;
		ServerEvent event;
		while (server.poll(1, now, event))
		{
		}

		TransportEvent transport_event;
		while (rogue.poll(now, transport_event))
		{
		}

		// Nothing but bulk, before any Hello.
		const std::vector<u8> bytes(100, 1);
		EXPECT_TRUE(rogue.send(to_server, bytes, Delivery::Bulk, now));

		now += DT;
		while (server.poll(2, now, event))
		{
		}

		now += DT;
		bool disconnected		= false;
		DisconnectReason reason = DisconnectReason::None;
		while (rogue.poll(now, transport_event))
		{
			if (transport_event.kind == TransportEventKind::Disconnected)
			{
				disconnected = true;
				reason		 = transport_event.reason;
			}
		}

		EXPECT_TRUE(disconnected);
		EXPECT_EQ(reason, DisconnectReason::Malformed);
	}

	TEST(Endpoints, BulkFromAServerBeforeTheWelcomeEndsTheSession)
	{
		LoopbackNetwork network;
		LoopbackTransport rogue_server{network};
		LoopbackTransport client_end{network};
		Client client{client_end, {.commands = command_codec<Input>(), .game_protocol = 7}};

		EXPECT_TRUE(rogue_server.listen("server"));
		EXPECT_TRUE(client.connect("server", 0.0));

		f64 now = DT;
		TransportEvent transport_event;
		PeerId to_client = NO_PEER;
		while (rogue_server.poll(now, transport_event))
			if (transport_event.kind == TransportEventKind::Connected)
				to_client = transport_event.peer;

		ASSERT_NE(to_client, NO_PEER);

		ClientEvent event;
		while (client.poll(now, event))
		{
		} // the client says Hello

		now += DT;
		while (rogue_server.poll(now, transport_event))
		{
		} // which this server ignores

		const std::vector<u8> bytes(10, 2);
		EXPECT_TRUE(rogue_server.send(to_client, bytes, Delivery::Bulk, now));

		now += DT;
		bool left				= false;
		DisconnectReason reason = DisconnectReason::None;
		while (client.poll(now, event))
		{
			if (event.kind == ClientEventKind::Left)
			{
				left   = true;
				reason = event.reason;
			}
		}

		EXPECT_TRUE(left);
		EXPECT_EQ(reason, DisconnectReason::Malformed);
	}
}
