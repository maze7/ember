#pragma once

#include <ember/core/result.h>
#include <ember/net/transport.h>
#include <ember/sync/spin_mutex.h>

#include <array>

namespace ember::net
{
	/**
	 * True when this build runs on Steam's SteamNetworkingSockets, from the Steamworks SDK, rather
	 * than GameNetworkingSockets, its open source twin: the EMBER_USE_STEAM CMake option.
	 *
	 * Both libraries implement one API, so everything below works on either; Steam adds connections
	 * to other Steam users by Steam ID, through its relay network.
	 */
	inline constexpr bool STEAM_NETWORKING = EMBER_USE_STEAM != 0;

	/**
	 * Simulated weather for every connection in the process that goes through a socket: connections
	 * over IP and through Steam's relays, and connect_local() through the network. The library applies
	 * it at its UDP layer: `send` to every datagram the process sends, `receive` to every one it
	 * receives. Both ends of an in-process session send, so `send` alone gives it the weather both ways
	 * and its round trip is twice the latency; a client of a server elsewhere needs both for the same.
	 * Its jitter is exponential where the loopback's is uniform: jitter here is its cap and half of it
	 * the mean. Its loss takes the library's own datagrams too, whose reliable messages it sends again.
	 * For development; default conditions turn it off.
	 */
	void simulate(const LinkConditions& send, const LinkConditions& receive = {}) noexcept;

	/**
	 * The transport games use: Valve's networking sockets, which bring the connection handshake,
	 * encryption, resends for reliable messages and pacing. Unreliable sends skip Nagle, so a packet
	 * leaves the moment the netcode hands it over.
	 *
	 * connect_local() joins two transports in one process without the network at all, which is how
	 * single player runs the same client and server code as a networked session.
	 *
	 * Arrival times come from the library's receive thread, so they mark when a datagram reached the
	 * machine, not when poll() noticed it.
	 *
	 * Every connection has two lanes: lane 0 for the game's packets and messages, and a bulk lane behind
	 * it, at lower priority, for Delivery::Bulk. The library sends lane 0 first whenever it has anything
	 * to send there, so a file on its way never delays a tick's packet, and a bulk message of up to
	 * MAX_BULK_BYTES travels whole, fragmented and reassembled by the library.
	 *
	 * One thread drives each transport. The library reports connection changes through a callback that
	 * whichever transport polls next runs for all of them (or, in a Steam build, the app's
	 * SteamAPI_RunCallbacks), so each transport's connection table is guarded by a lock of its own.
	 */
	class ValveTransport final : public Transport
	{
	public:
		/** A transport made before net::initialize() refuses everything with NotInitialized. */
		ValveTransport() noexcept;

		/** Closes every connection: peers see Disconnected (Shutdown) */
		~ValveTransport() noexcept override;

		/**
		 * Starts accepting connections, at one of:
		 *
		 *     "ip:port", "[ipv6]:port"   UDP on that address
		 *     ":port"                    UDP on that port, every interface
		 *     "steam", "steam:<port>"    Steam builds: connections to this user's (or server's) Steam ID
		 *                                through Steam's relays, on a virtual port, 0 by default
		 *
		 * UDP needs a port: 0 is a BadAddress, a port in use is AddressInUse. A Steam address in a
		 * GameNetworkingSockets build is Unsupported.
		 */
		[[nodiscard]] Result<void, TransportError> listen(StringView address) noexcept override;

		/**
		 * Connects to one of:
		 *
		 *     "ip:port", "[ipv6]:port"                 UDP
		 *     "steam:<steam id>", "steam:<id>:<port>"  Steam builds: that Steam user or server, through
		 *                                              Steam's relays, at a virtual port, 0 by default
		 */
		[[nodiscard]] Result<PeerId, TransportError> connect(StringView address, f64 now) noexcept override;

		/**
		 * Connects to a server, a transport in this process, without the network: the library hands messages
		 * straight across, unencrypted and never lost (single player). through_network routes them over
		 * 127.0.0.1 through the whole stack isntead, where simulate() applies: single player under real lag
		 * and loss. Both ends hear Connected from their next poll().
		 */
		[[nodiscard]] Result<PeerId, TransportError> connect_local(ValveTransport& server, bool through_network,
																   f64 now) noexcept;

		void disconnect(PeerId peer, DisconnectReason reason, f64 now) noexcept override;
		bool send(PeerId peer, Span<const u8> data, Delivery delivery, f64 now) noexcept override;
		bool poll(f64 now, TransportEvent& event) noexcept override;
		PeerStats stats(PeerId peer) const noexcept override;

		/**
		 * The peer's Steam ID, as Steam authenticated it: who is really on the other end, for bans, friends
		 * and lobbies. 0 when there is none: a GameNetworkingSockets build, or a peer that proved no Steam
		 * identity.
		 */
		u64 steam_id(PeerId peer) const noexcept;

		/** The UDP port listen() bound; 0 before it, or when listening through Steam. */
		u16 listen_port() const noexcept;

		/** Connections open or opening this end. */
		u32 connection_count() const noexcept;

	private:
		struct Callbacks; // defined in valve_trasport.cpp

		friend Result<void, TransportError> initialize(const NetDef& def) noexcept; // installs the callback

		struct Peer
		{
			PeerId id	   = NO_PEER;
			u32 connection = 0;		// HSteamNetConnection
			bool connected = false; // Connected has been reported
			bool incoming  = false; // accepted from the listen socket, rather than connected to
		};

		struct Status
		{
			TransportEventKind kind = TransportEventKind::Connected;
			PeerId peer				= NO_PEER;
			DisconnectReason reason = DisconnectReason::None;
		};

		bool ready() const noexcept;
		u32 connection_of(PeerId peer) const noexcept;
		PeerId add_peer(u32 connection, bool connected, bool incoming) noexcept;
		void connected(u32 connection) noexcept;
		void closed(u32 connection, DisconnectReason reason) noexcept;

		u32 m_listen_socket = 0; // HSteamListenSocket
		u32 m_poll_group	= 0; // HSteamNetPollGroup: every connection of this transport, for receiving.
		PeerId m_next_peer	= 1;

		// Under m_lock: library callbacks for this transport may run on another thread.
		mutable SpinMutex m_lock;
		Vector<Peer> m_peers;
		Vector<Status> m_status; // connection changes waiting for poll(), oldest first

		/** The bytes poll() handed out last; the event's span points here until the next poll(). */
		std::array<u8, MAX_PACKET_BYTES> m_received = {};

		/**
		 * The library's own message behind the bulk event poll() handed out last, released at the next
		 * poll(): a bulk message is read where the library put it rather than copied. Its type is the
		 * library's, which no header of the engine names.
		 */
		void* m_held = nullptr;
	};
}
