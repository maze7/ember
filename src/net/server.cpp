#include <ember/memory/memory.h>
#include <ember/net/server.h>

#include <algorithm>
#include <cmath>

namespace ember::net
{
	namespace
	{
		constexpr f64 HELD_UNIT = 0.0005; // seconds per step of Welcome::held: half a millisecond
	}

	Server::Server(Transport& transport, const ServerDef& def) noexcept
		: m_transport(transport), m_def(def), m_clients(&memory::heap(MemoryTag::Network)),
		  m_greeting(&memory::heap(MemoryTag::Network))
	{
		EMBER_ASSERT(def.max_clients > 0);
		EMBER_ASSERT((def.replicator == nullptr || def.replicator->viewer_count() >= def.max_clients) &&
					 "the replicator needs a viewer for every seat: ReplicatorDef::max_viewers");

		// Every seat exists from the start, so a client's state never moves.
		m_clients.reserve(def.max_clients);
		for (u32 slot = 0; slot < def.max_clients; ++slot)
			m_clients.push_back(
				{.connection = Connection(def.connection), .commands = CommandQueue(def.commands, def.queue)});

		m_greeting.reserve(def.max_greeting);
	}

	Server::~Server() noexcept
	{
		for (const Greeting& greeting : m_greeting)
			m_transport.disconnect(greeting.peer, DisconnectReason::Shutdown, m_now);

		for (Client& client : m_clients)
		{
			if (client.playing)
				m_transport.disconnect(client.peer, DisconnectReason::Shutdown, m_now);
		}
	}

	Result<void, TransportError> Server::listen(StringView address) noexcept { return m_transport.listen(address); }

	bool Server::poll(Tick tick, f64 now, ServerEvent& event) noexcept
	{
		m_tick = tick;
		m_now  = now;

		TransportEvent incoming;
		while (m_transport.poll(now, incoming))
		{
			switch (incoming.kind)
			{
				case TransportEventKind::Connected:
					connected(incoming.peer, now);
					break;

				case TransportEventKind::Disconnected:
					if (Client* client = client_of(incoming.peer); client != nullptr)
					{
						// The transport has let the peer go already: only the seat is left to free.
						event = {.kind = ServerEventKind::Left, .slot = slot_of(*client), .reason = incoming.reason};
						leave(*client);
						return true;
					}

					std::erase_if(m_greeting, [&](const Greeting& greeting) { return greeting.peer == incoming.peer; });
					break;

				case TransportEventKind::Received:
					if (incoming.delivery == Delivery::Reliable)
					{
						if (received_message(incoming, now, event))
							return true;
					}
					else if (Client* client = client_of(incoming.peer); client != nullptr)
					{
						received_packet(*client, incoming);
					}
					break;

				case TransportEventKind::Count:
					break;
			}
		}

		return drop_the_silent(now, event);
	}

	void Server::send_packets(f64 now) noexcept
	{
		m_now = now;

		// The world as the tick left it, once, for every client's packet.
		if (m_def.replicator != nullptr && m_tick != NO_TICK)
			m_def.replicator->update(m_tick);

		for (Client& client : m_clients)
		{
			if (!client.playing)
				continue;

			// The replicator learns which of its records arrived
			PacketNotice notice;
			while (client.connection.take_notice(notice))
			{
				if (m_def.replicator != nullptr)
					m_def.replicator->on_notice(slot_of(client), notice);
			}

			PacketBuffer buffer;
			serialize::WriteStream stream = packet_writer(buffer);
			const Sequence sequence		  = client.connection.write_header(stream, now);
			write_command_timing(stream, client.commands);

			// The entity section, as the world stands after the tick: present or not.
			stream.SerializeBits(m_def.replicator != nullptr ? 1u : 0u, 1);
			if (m_def.replicator != nullptr)
				m_def.replicator->write(slot_of(client), stream, sequence, m_tick);
			stream.Flush();

			const Span<const u8> bytes = written(buffer, stream);
			m_transport.send(client.peer, bytes, Delivery::Unreliable, now);
			client.connection.count_sent(static_cast<u32>(bytes.size()), now);
		}
	}

	void Server::send_message(u8 slot, Span<const u8> data, f64 now) noexcept
	{
		EMBER_ASSERT(data.size() <= MAX_MESSAGE_BYTES && "larger than a message carries");

		if (!playing(slot) || data.size() > MAX_MESSAGE_BYTES)
			return;

		Message message = {.kind = MessageKind::Game, .size = static_cast<u32>(data.size())};
		std::copy(data.begin(), data.end(), message.payload.begin());
		net::send_message(m_transport, m_clients[slot].peer, message, now);
	}

	void Server::kick(u8 slot, DisconnectReason reason, f64 now) noexcept
	{
		if (!playing(slot))
			return;

		m_transport.disconnect(m_clients[slot].peer, reason, now);
		leave(m_clients[slot]);
	}

	bool Server::playing(u8 slot) const noexcept { return slot < m_clients.size() && m_clients[slot].playing; }

	u32 Server::client_count() const noexcept
	{
		return static_cast<u32>(
			std::count_if(m_clients.begin(), m_clients.end(), [](const Client& client) { return client.playing; }));
	}

	PeerId Server::peer(u8 slot) const noexcept { return playing(slot) ? m_clients[slot].peer : NO_PEER; }

	const ConnectionStats* Server::stats(u8 slot) const noexcept
	{
		return playing(slot) ? &m_clients[slot].connection.stats() : nullptr;
	}

	const CommandTiming* Server::timing(u8 slot) const noexcept
	{
		return playing(slot) ? &m_clients[slot].commands.timing() : nullptr;
	}

	void Server::connected(PeerId peer, f64 now) noexcept
	{
		// Peers that connect and never speak cost a little each: only so many may wait at once.
		if (m_greeting.size() >= m_def.max_greeting)
		{
			m_transport.disconnect(peer, DisconnectReason::Rejected, now);
			return;
		}

		m_greeting.push_back({.peer = peer, .since = now});
	}

	bool Server::received_message(const TransportEvent& incoming, f64 now, ServerEvent& event) noexcept
	{
		Client* client = client_of(incoming.peer);

		if (!read_message(incoming.data, m_message))
		{
			if (client != nullptr)
				return drop(*client, DisconnectReason::Malformed, now, event);

			refuse(incoming.peer, DisconnectReason::Malformed, now);
			return false;
		}

		if (client == nullptr)
		{
			// Before the Welcome, a Hello is the only thing a client may say.
			if (greeting_of(incoming.peer) == nullptr)
				return false;

			if (m_message.kind != MessageKind::Hello)
			{
				refuse(incoming.peer, DisconnectReason::Malformed, now);
				return false;
			}

			const Hello& hello = m_message.hello;
			if (hello.engine_protocol != PROTOCOL_VERSION || hello.game_protocol != m_def.game_protocol)
			{
				refuse(incoming.peer, DisconnectReason::ProtocolMismatch, now);
				return false;
			}

			welcome(incoming.peer, incoming.time, now);

			client = client_of(incoming.peer);
			if (client == nullptr)
				return false; // refused: no seat

			event = {.kind = ServerEventKind::Joined, .slot = slot_of(*client)};
			return true;
		}

		// A welcomed client says Hello once; anything but game messages from here is a broken peer.
		if (m_message.kind != MessageKind::Game)
			return drop(*client, DisconnectReason::Malformed, now, event);

		event = {.kind = ServerEventKind::Message,
				 .slot = slot_of(*client),
				 .data = Span<const u8>(m_message.payload.data(), m_message.size)};
		return true;
	}

	void Server::received_packet(Client& client, const TransportEvent& incoming) noexcept
	{
		PacketBuffer buffer;
		serialize::ReadStream stream;
		if (!packet_reader(stream, buffer, incoming.data))
			return;

		Sequence sequence = 0;
		if (client.connection.read_header(stream, sequence, incoming.time) != Connection::Arrival::Fresh)
			return;

		// A packet that does not decode is never acknowledged: to the client it is lost.
		if (client.commands.read(stream, incoming.time))
			client.connection.acknowledge(sequence, static_cast<u32>(incoming.data.size()), incoming.time);
	}

	bool Server::drop_the_silent(f64 now, ServerEvent& event) noexcept
	{
		for (u32 i = 0; i < m_greeting.size();)
		{
			if (now - m_greeting[i].since > m_def.handshake_timeout)
			{
				m_transport.disconnect(m_greeting[i].peer, DisconnectReason::TimedOut, now);
				m_greeting.erase(m_greeting.begin() + i);
			}
			else
			{
				++i;
			}
		}

		for (Client& client : m_clients)
		{
			if (client.playing && client.connection.timed_out(now))
				return drop(client, DisconnectReason::TimedOut, now, event);
		}

		return false;
	}

	void Server::welcome(PeerId peer, f64 hello_arrival, f64 now) noexcept
	{
		std::erase_if(m_greeting, [peer](const Greeting& greeting) { return greeting.peer == peer; });

		const auto seat = std::find_if(m_clients.begin(), m_clients.end(), [](const Client& c) { return !c.playing; });
		if (seat == m_clients.end())
		{
			refuse(peer, DisconnectReason::ServerFull, now);
			return;
		}

		// A fresh seat: numbering, acks and the arrival measurement all start over.
		Client& client = *seat;
		client.playing = true;
		client.peer	   = peer;
		client.connection.reset(now);
		client.commands.reset();

		if (m_def.replicator != nullptr)
			m_def.replicator->add_viewer(slot_of(client));

		const f64 held	= std::round((now - hello_arrival) / HELD_UNIT);
		Message message = {.kind	= MessageKind::Welcome,
						   .welcome = {.slot		= slot_of(client),
									   .server_tick = m_tick,
									   .held		= static_cast<u16>(std::clamp(held, 0.0, 65535.0))}};
		net::send_message(m_transport, peer, message, now);
	}

	void Server::refuse(PeerId peer, DisconnectReason reason, f64 now) noexcept
	{
		std::erase_if(m_greeting, [peer](const Greeting& greeting) { return greeting.peer == peer; });
		m_transport.disconnect(peer, reason, now);
	}

	bool Server::drop(Client& client, DisconnectReason reason, f64 now, ServerEvent& event) noexcept
	{
		event = {.kind = ServerEventKind::Left, .slot = slot_of(client), .reason = reason};
		m_transport.disconnect(client.peer, reason, now);
		leave(client);
		return true;
	}

	void Server::leave(Client& client) noexcept
	{
		if (m_def.replicator != nullptr)
			m_def.replicator->remove_viewer(slot_of(client));

		client.playing = false;
		client.peer	   = NO_PEER;
	}

	u8 Server::slot_of(const Client& client) const noexcept { return static_cast<u8>(&client - m_clients.data()); }

	Server::Client* Server::client_of(PeerId peer) noexcept
	{
		for (Client& client : m_clients)
		{
			if (client.playing && client.peer == peer)
				return &client;
		}

		return nullptr;
	}

	Server::Greeting* Server::greeting_of(PeerId peer) noexcept
	{
		for (Greeting& greeting : m_greeting)
		{
			if (greeting.peer == peer)
				return &greeting;
		}

		return nullptr;
	}
}
