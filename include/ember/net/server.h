#pragma once

#include <ember/net/command_stream.h>
#include <ember/net/connection.h>
#include <ember/net/messages.h>
#include <ember/net/replicator.h>
#include <ember/net/transport.h>

namespace ember::net
{
	struct ServerDef
	{
		CommandCodec commands	 = {};		// the game's command type: command_codec<T>()
		u32 game_protocol		 = 0;		// the game's wire version: a client with another is refused
		u8 max_clients			 = 16;		// seats; a client that finds none is refused as ServerFull
		u8 max_greeting			 = 16;		// peers connected but not yet welcomed, at most: more are Rejected
		f64 handshake_timeout	 = 5.0;		// seconds a connected peer has to say Hello
		ConnectionDef connection = {};		// per client: the silence after which it is dropped
		CommandQueueDef queue	 = {};		// per client: the tick length and how arrival times are smoothed
		Replicator* replicator	 = nullptr; // the server world's entities, one viewer per seat; null sends none
	};

	enum class ServerEventKind : u8
	{
		Joined,	 // a client was welcomed into a slot: its commands start arriving
		Left,	 // a client is gone, the slot is free again
		Message, // a client's game message, delivered once and in order
		Count
	};

	struct ServerEvent
	{
		ServerEventKind kind	= ServerEventKind::Message;
		u8 slot					= 0;
		DisconnectReason reason = DisconnectReason::None; // Why they left
		Span<const u8> data		= {};					  // Message: the bytes, valid until the next poll()
	};

	/**
	 * The Server end the network layer: the handshake, one connection and command queue per seat, the packet
	 * each client gets every tick, the game's reliable messages, and dropping whoever goes quiet. The game
	 * sees seats (slots), never transport peers.
	 *
	 * A connection is welcomed in three steps. The transport connects; the client says Hello with the
	 * engine's and the game's protocol versions; the server checks both, finds a seat, and answers Welcome
	 * with the seat and its current clock. A client that speaks another protocol is refused as ProtocolMismatch,
	 * one that finds no seat as ServerFull, one that never says Hello is dropped after handshake_timeout, and
	 * while max_greeting peers wait, the next is Rejected at the door.
	 *
	 * Only a welcomed client's packets count.
	 *
	 * Each tick the game polls with the tick it is about to simulate (packets in, events out), takes every seat's
	 * command for that tick, simulates it, and calls send_packets: each client gets the connection header, the
	 * report on how early its commands arrive, which steers its clock, and the entities the replicator owes it
	 * as the world stands after the tick: send_packets() has the replicator look at it first.
	 *
	 * Single threaded: the server's game thread drives it. The transport must outlive it.
	 */
	class Server final
	{
	public:
		Server(Transport& transport, const ServerDef& def) noexcept;

		/** Closes every connection:l clients see Left (Shutdown). */
		~Server() noexcept;

		Server(const Server&)			 = delete;
		Server& operator=(const Server&) = delete;

		/** Starts accepting clients, at an address the transport understands. */
		[[nodiscard]] Result<void, TransportError> listen(StringView address) noexcept;

		/**
		 * The next event; false once there is none. Reads every Packet and message the transport has, runs the
		 * handshake, and drops  peers that went quiet. Call it until false at the start of every tick, with the
		 * tick about ot be simulated: a client welcomed now starts from it.
		 */
		[[nodiscard]] bool poll(Tick tick, f64 now, ServerEvent& event) noexcept;

		/**
		 * The command a seat's client made for the tick: its own if it arrived in time, else a stand-in
		 * (command_stream.h). Each seat's ticks are taken in order, each once, as the tick is simulated.
		 */
		template <class T> TakenCommand take_command(u8 slot, Tick tick, T& out, f64 now) noexcept
		{
			if (!playing(slot))
			{
				out = T{};
				return {};
			}

			return m_clients[slot].commands.take(tick, out, now);
		}

		/** After simulating a tick: every welcomed client gets its packet. */
		void send_packets(f64 now) noexcept;

		/** A game message for a seat's client: reliable, in order. At most MAX_MESSAGE_BYTES. */
		void send_message(u8 slot, Span<const u8> data, f64 now) noexcept;

		/** Removes a seat's client; it sees Left with the reason. No Left event follows here. */
		void kick(u8 slot, DisconnectReason reason, f64 now) noexcept;

		/** True while a welcomed client holds the seat. */
		bool playing(u8 slot) const noexcept;

		/** Welcomed clients. */
		u32 client_count() const noexcept;

		/** The transport's peer for a seat, NO_PEER when empty: for the transport's own stats and identity. */
		PeerId peer(u8 slot) const noexcept;

		/** A seat's connection statistics; null when empty. */
		const ConnectionStats* stats(u8 slot) const noexcept;

		/** How early a seat's commands arrive; null when empty. */
		const CommandTiming* timing(u8 slot) const noexcept;

	private:
		struct Client
		{
			bool playing = false;
			PeerId peer	 = NO_PEER;
			Connection connection;
			CommandQueue commands;
		};

		struct Greeting
		{
			PeerId peer = NO_PEER;
			f64 since	= 0.0; // when the transport connected
		};

		void connected(PeerId peer, f64 now) noexcept;
		[[nodiscard]] bool received_message(const TransportEvent& incoming, f64 now, ServerEvent& event) noexcept;
		void received_packet(Client& client, const TransportEvent& incoming) noexcept;
		[[nodiscard]] bool drop_the_silent(f64 now, ServerEvent& event) noexcept;
		void welcome(PeerId peer, f64 hello_arrival, f64 now) noexcept;
		void refuse(PeerId peer, DisconnectReason reason, f64 now) noexcept;

		/** Closes a seat's connection with the reason, frees the seat, and reports Left. */
		[[nodiscard]] bool drop(Client& client, DisconnectReason reason, f64 now, ServerEvent& event) noexcept;

		/** Frees a seat whose connection is already closed. */
		void leave(Client& client) noexcept;

		[[nodiscard]] u8 slot_of(const Client& client) const noexcept;
		[[nodiscard]] Client* client_of(PeerId peer) noexcept;
		[[nodiscard]] Greeting* greeting_of(PeerId peer) noexcept;

		Transport& m_transport;
		ServerDef m_def;
		Tick m_tick = NO_TICK; // the tick poll() was given: what a Welcome reports
		f64 m_now	= 0.0;	   // the latest time the game gave, for the destructor's goodbyes

		Vector<Client> m_clients; // one per seat; the index is the slot
		Vector<Greeting> m_greeting;

		/** The message poll() handed out last; the event's span points into it until the next poll(). */
		Message m_message;
	};
}

namespace ember
{
	EMBER_ENUM_NAMES(net::ServerEventKind, "Joined", "Left", "Message");
}
