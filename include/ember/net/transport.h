#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/core/result.h>
#include <ember/net/sequence.h>

namespace ember::net
{
	/**
	 * A transport's name for one connection. Never reused for the transport's lifeimte, so a stale
	 * id can never reach a newer peer.
	 */
	using PeerId = u32;

	inline constexpr PeerId NO_PEER = 0;

	enum class Role : u8
	{
		Client, // the app is a game client, hosting or not
		Server, // the app is a dedicated server
		Count
	};

	struct NetDef
	{
		Role role = Role::Client;
	};

	enum class TransportError : u8
	{
		AlreadyInitialized, // the library behidn the transport is already running
		NotInitialized,		// the library behind the transport is not running: call net::initialize()
		LibraryFailed,		// the underlying library (GNS / SteamNetworkingSockets) failed to init
		SteamNotRunning,	// Steam builds: Steam has not been started, or not in the role asked for
		BadAddress,			// the address does not parse for this transport
		AlreadyListening,	// listen() called twice
		AddressInUse,		// another endpoint holds the address
		Unreachable,		// nothing listens there, so the route is down
		Unsupported,		// the requested feature is not supported in the compiled transport
		Count
	};

	/** Why a connection ended. Travels between peers, so append only. */
	enum class DisconnectReason : u8
	{
		None,			  // still connected, or never was
		Requested,		  // this side or the peer asked to close
		TimedOut,		  // nothing arrived for longer than the connection's timeout
		Rejected,		  // the server refused the connection
		ProtocolMismatch, // the two builds do not speak the same protocol
		ServerFull,		  // no free slot on the server
		Kicked,			  // removed by the server
		Malformed,		  // the peer sent data that does not decode
		Transport,		  // the transport lost the connection for its own reasons
		Shutdown,		  // the endpoint is stopping
		Count
	};

	/** How a send travels. */
	enum class Delivery : u8
	{
		Unreliable, // may be lost or arrive out of order: the protocol's own packets, wich acks track
		Reliable,	// arrives once and in order with the other reliable sends: the session's messages
		Count,
	};

	enum class TransportEventKind : u8
	{
		Connected,	  // a connection is up: accepted on a server, established on a client
		Disconnected, // a connection is gone; the peer ID is dead from here on
		Received,	  // one datagram or message arrived
		Count
	};

	struct TransportEvent
	{
		TransportEventKind kind = TransportEventKind::Received;
		PeerId peer				= NO_PEER;

		/**
		 * When the event happened, on the caller's clock. For a datagram this is its arrival, which
		 * a transport with a receive thread of its own (GameNetworkingSockets) stamps earlier than
		 * poll() hands it over; the connection measures round trips from it.
		 */
		f64 time = 0.0;

		Span<const u8> data		= {};					  // Received: the bytes, valid until the next poll()
		Delivery delivery		= Delivery::Unreliable;	  // Received: how they travelled
		DisconnectReason reason = DisconnectReason::None; // Disconnected: why
	};

	/**
	 * Network weather to simulate on the way out of a transport. Used to stress test the netcode of a game
	 * during development under poor conditions.
	 */
	struct LinkConditions
	{
		f64 latency	  = 0.0;   // seconds, one way
		f64 jitter	  = 0.0;   // extra seconds on top, up to this much
		f32 loss	  = 0.0f;  // chance an unreliable datagram vanishes
		f32 duplicate = 0.0f;  // chance an unreliable datagram arrives twice
		bool reorder  = false; // let datagrams overtake each other; off, they arrive in the order sent
	};

	/** What a Transport can tell about one connection's path. Zero when it cannot say. */
	struct PeerStats
	{
		f32 ping			   = 0.0f; // seconds, the transport's own round trip estimate
		f32 quality_local	   = 0.0f; // fraction of the peer's packets that arrived here, in order
		f32 quality_remote	   = 0.0f; // the same, as the peer sees this end's packets
		f32 send_rate		   = 0.0f; // bytes per second the transport will put on the wire before queueing
		f32 bytes_out		   = 0.0f; // bytes per second sent, recently
		f32 bytes_in		   = 0.0f; // bytes per second received, recently
		f32 queue_time		   = 0.0f; // seconds a send made now would wait before leaving
		u32 pending_unreliable = 0;
		u32 pending_reliable   = 0;
	};

	/** Starts the networking layer for the process. */
	[[nodiscard]] Result<void, TransportError> initialize(const NetDef& def = {}) noexcept;

	/** Stops the library. Every Transport must be destroyed first. */
	void shutdown() noexcept;

	/** True between initialize() and shutdown() */
	bool is_initialized() noexcept;

	class Transport
	{
	public:
		virtual ~Transport() noexcept = default;

		/** Starts accepting connections at address. Server side; once. */
		[[nodiscard]] virtual Result<void, TransportError> listen(StringView addres) noexcept = 0;

		/**
		 * Starts connecting to address. Client side. Connected or Disconnected follows from poll().
		 * Sends before Connected may be dropped.
		 */
		[[nodiscard]] virtual Result<PeerId, TransportError> connect(StringView address, f64 now) noexcept = 0;

		/** Closes a connection. The peer sees Disconnected with the reason; this end sees nothing. */
		virtual void disconnect(PeerId peer, DisconnectReason reason, f64 now) noexcept = 0;

		/** Sends up to MAX_PACKET_BYTES. A send to an unknown or closed peer is dropped. */
		virtual void send(PeerId peer, Span<const u8> data, Delivery delivery, f64 now) noexcept = 0;

		/** The next event due by now; false once there is none. Its data lives until the next call. */
		virtual bool poll(f64 now, TransportEvent& event) noexcept = 0;

		/** The path to a peer as the Transport sees it; zeros for an unknown peer. */
		virtual PeerStats stats(PeerId peer) const noexcept = 0;

	protected:
		Transport() noexcept				   = default;
		Transport(const Transport&)			   = delete;
		Transport& operator=(const Transport&) = delete;
	};
}

namespace ember
{
	EMBER_ENUM_NAMES(net::Role, "Client", "Server");
	EMBER_ENUM_NAMES(net::TransportError, "AlreadyInitialized", "NotInitialized", "LibraryFailed", "SteamNotRunning",
					 "BadAddress", "AlreadyListening", "AddressInUse", "Unreachable", "Unsupported");
	EMBER_ENUM_NAMES(net::Delivery, "Unreliable", "Reliable");
	EMBER_ENUM_NAMES(net::TransportEventKind, "Connected", "Disconnected", "Received");
	EMBER_ENUM_NAMES(net::DisconnectReason, "None", "Requested", "TimedOut", "Rejected", "ProtocolMismatch",
					 "ServerFull", "Kicked", "Malformed", "Transport", "Shutdown");
}
