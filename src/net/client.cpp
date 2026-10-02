#include <ember/net/client.h>

#include <algorithm>

namespace ember::net
{
	namespace
	{
		constexpr f64 HELD_UNIT = 0.0005; // seconds per step of Welcome::held: half a millisecond
	}

	Client::Client(Transport& transport, const ClientDef& def) noexcept
		: m_transport(transport), m_def(def), m_connection(def.connection), m_commands(def.commands),
		  m_dilation(def.dilation)
	{
		EMBER_ASSERT(def.tick_seconds > 0.0);
	}

	Client::~Client() noexcept
	{
		if (m_peer != NO_PEER)
			m_transport.disconnect(m_peer, DisconnectReason::Shutdown, m_now);
	}

	Result<void, TransportError> Client::connect(StringView address, f64 now) noexcept
	{
		EMBER_ASSERT(m_state == ClientState::Idle && "disconnect before connecting again");

		Result<PeerId, TransportError> peer = m_transport.connect(address, now);
		if (!peer)
			return fail(peer.error());

		m_peer		   = peer.value();
		m_state		   = ClientState::Connecting;
		m_connect_time = now;
		m_now		   = now;
		return {};
	}

	void Client::attach(PeerId peer, f64 now) noexcept
	{
		EMBER_ASSERT(m_state == ClientState::Idle && "disconnect before connecting again");
		EMBER_ASSERT(peer != NO_PEER);

		m_peer		   = peer;
		m_state		   = ClientState::Connecting;
		m_connect_time = now;
		m_now		   = now;
	}

	void Client::disconnect(DisconnectReason reason, f64 now) noexcept
	{
		if (m_peer != NO_PEER)
			m_transport.disconnect(m_peer, reason, now);

		m_peer	= NO_PEER;
		m_state = ClientState::Idle;
	}

	bool Client::poll(f64 now, ClientEvent& event) noexcept
	{
		m_now = now;

		TransportEvent incoming;
		while (m_transport.poll(now, incoming))
		{
			if (incoming.peer != m_peer || m_peer == NO_PEER)
				continue;

			switch (incoming.kind)
			{
				case TransportEventKind::Connected:
					if (m_state == ClientState::Connecting)
					{
						// The first word on the reliable channel: which protocols this build speaks.
						const Message hello = {.kind  = MessageKind::Hello,
											   .hello = {.game_protocol = m_def.game_protocol}};
						net::send_message(m_transport, m_peer, hello, now);

						m_hello_time = now;
						m_state		 = ClientState::Greeting;
					}
					break;

				case TransportEventKind::Disconnected:
					// The transport has let the peer go already: nothing to close, only the state to clear.
					m_peer = NO_PEER;
					return leave(incoming.reason, now, event);

				case TransportEventKind::Received:
					if (incoming.delivery == Delivery::Reliable)
					{
						if (received_message(incoming, now, event))
							return true;
					}
					else if (m_state == ClientState::Playing)
					{
						received_packet(incoming);
					}
					break;

				case TransportEventKind::Count:
					break;
			}
		}

		const bool greeting = m_state == ClientState::Connecting || m_state == ClientState::Greeting;
		if (greeting && now - m_connect_time > m_def.handshake_timeout)
			return leave(DisconnectReason::TimedOut, now, event);

		if (m_state == ClientState::Playing && m_connection.timed_out(now))
			return leave(DisconnectReason::TimedOut, now, event);

		return false;
	}

	void Client::send_message(Span<const u8> data, f64 now) noexcept
	{
		EMBER_ASSERT(data.size() <= MAX_MESSAGE_BYTES && "larger than a message carries");

		if (m_state != ClientState::Playing || data.size() > MAX_MESSAGE_BYTES)
			return;

		Message message = {.kind = MessageKind::Game, .size = static_cast<u32>(data.size())};
		std::copy(data.begin(), data.end(), message.payload.begin());
		net::send_message(m_transport, m_peer, message, now);
	}

	void Client::send_packet(f64 now) noexcept
	{
		PacketBuffer buffer;
		serialize::WriteStream stream = packet_writer(buffer);

		const Sequence sequence = m_connection.write_header(stream, now);
		m_commands.write(stream, sequence, m_dilation.epoch());
		stream.Flush();

		const Span<const u8> bytes = written(buffer, stream);
		m_transport.send(m_peer, bytes, Delivery::Unreliable, now);
		m_connection.count_sent(static_cast<u32>(bytes.size()), now);
	}

	void Client::received_packet(const TransportEvent& incoming) noexcept
	{
		PacketBuffer buffer;
		serialize::ReadStream stream;
		if (!packet_reader(stream, buffer, incoming.data))
			return;

		Sequence sequence				  = 0;
		const Connection::Arrival arrival = m_connection.read_header(stream, sequence, incoming.time);

		// The acks a header carries count whatever the verdict: tell the command stream what arrived.
		PacketNotice notice;
		while (m_connection.take_notice(notice))
			m_commands.on_notice(notice);

		if (arrival != Connection::Arrival::Fresh)
			return;

		CommandTiming timing;
		bool present = false;
		if (!read_command_timing(stream, timing, present))
			return;

		if (present)
			m_dilation.report(timing);

		m_connection.acknowledge(sequence, static_cast<u32>(incoming.data.size()), incoming.time);
	}

	bool Client::received_message(const TransportEvent& incoming, f64 now, ClientEvent& event) noexcept
	{
		if (!read_message(incoming.data, m_message))
			return leave(DisconnectReason::Malformed, now, event);

		if (m_state == ClientState::Greeting && m_message.kind == MessageKind::Welcome)
		{
			const Welcome& welcome = m_message.welcome;

			// The handshake's round trip, less the time the server sat on the Hello: the path alone.
			const f64 rtt = std::max(incoming.time - m_hello_time - welcome.held * HELD_UNIT, 0.0);

			// A fresh session: numbering, the command stream and the clock all start over.
			m_connection.reset(incoming.time);
			m_commands.reset();
			m_dilation.reset();

			m_slot	= welcome.slot;
			m_state = ClientState::Playing;

			event = {.kind = ClientEventKind::Joined,
					 .slot = welcome.slot,
					 .tick = initial_client_tick(welcome.server_tick, rtt, m_def.tick_seconds, m_def.dilation)};
			return true;
		}

		if (m_state == ClientState::Playing && m_message.kind == MessageKind::Game)
		{
			event = {.kind = ClientEventKind::Message,
					 .data = Span<const u8>(m_message.payload.data(), m_message.size)};
			return true;
		}

		// A Welcome twice, a Hello from the server, a game message before the Welcome: a broken server.
		return leave(DisconnectReason::Malformed, now, event);
	}

	bool Client::leave(DisconnectReason reason, f64 now, ClientEvent& event) noexcept
	{
		disconnect(reason, now);
		event = {.kind = ClientEventKind::Left, .reason = reason};
		return true;
	}
}
