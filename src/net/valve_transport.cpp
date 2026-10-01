#include <ember/core/logger.h>
#include <ember/memory/memory.h>
#include <ember/net/valve_transport.h>
#include <ember/sync/thread.h>

#if EMBER_USE_STEAM_NETWORKING
	#include <steam/steam_api.h>
	#include <steam/steam_gameserver.h>
#else
	#include <steam/steamnetworkingsockets.h>
#endif

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

namespace ember::net
{
	static_assert(sizeof(HSteamNetConnection) == sizeof(u32) && sizeof(HSteamListenSocket) == sizeof(u32) &&
					  sizeof(HSteamNetPollGroup) == sizeof(u32),
				  "ValveTransport:w keeps the library's handles as u32, so sockets.h needs none of its headers");

	namespace
	{
		constexpr const char* LIBRARY = STEAM_NETWORKING ? "SteamNetworkingSockets" : "GameNetworkingSockets";

		std::atomic<bool> s_claimed = false; // initialize() has run, or is running

		/** What every transport talks through; null until networking is up. */
		std::atomic<ISteamNetworkingSockets*> s_sockets = nullptr;

		/**
		 * The library's status callback carries no context, so it finds its transport here: by
		 * connection, or for a connection still being accepted, by the listen socket it arrived on.
		 * Every handle is registered under the lock before the library can dispatch a callback for it,
		 * since dispatch takes the lock too. The library holds none of its own locks while it
		 * dispatches.
		 */
		struct Route
		{
			u32 handle				  = 0;
			bool listen				  = false;
			ValveTransport* transport = nullptr;
		};

		constexpr u32 MAX_ROUTES = 1024; // connections and listen sockets, every transport in the process

		std::mutex s_routes_lock;
		std::array<Route, MAX_ROUTES> s_routes = {};
		u32 s_route_count					   = 0;

		[[nodiscard]] bool add_route(u32 handle, bool listen, ValveTransport* transport) noexcept
		{
			EMBER_ASSERT(s_route_count < MAX_ROUTES && "more connections than the route table holds");

			if (s_route_count == MAX_ROUTES)
				return false;

			s_routes[s_route_count++] = {.handle = handle, .listen = listen, .transport = transport};
			return true;
		}

		void remove_route(u32 handle, bool listen) noexcept
		{
			for (u32 i = 0; i < s_route_count; ++i)
			{
				if (s_routes[i].handle == handle && s_routes[i].listen == listen)
				{
					s_routes[i] = s_routes[--s_route_count];
					return;
				}
			}
		}

		void remove_routes(const ValveTransport* transport) noexcept
		{
			for (u32 i = 0; i < s_route_count;)
			{
				if (s_routes[i].transport == transport)
					s_routes[i] = s_routes[--s_route_count];
				else
					++i;
			}
		}

		[[nodiscard]] ValveTransport* find_route(u32 handle, bool listen) noexcept
		{
			for (u32 i = 0; i < s_route_count; ++i)
			{
				if (s_routes[i].handle == handle && s_routes[i].listen == listen)
					return s_routes[i].transport;
			}

			return nullptr;
		}

		[[nodiscard]] ISteamNetworkingSockets& sockets() noexcept { return *s_sockets.load(std::memory_order_acquire); }

		// Our reasons travel in the library's application range, so the far end reads them back exactly.
		[[nodiscard]] int end_code(DisconnectReason reason) noexcept
		{
			return k_ESteamNetConnectionEnd_App_Min + static_cast<int>(reason);
		}

		[[nodiscard]] DisconnectReason reason_of(int code) noexcept
		{
			const int ours = code - k_ESteamNetConnectionEnd_App_Min;
			if (ours > 0 && ours < static_cast<int>(DisconnectReason::Count))
				return static_cast<DisconnectReason>(ours);

			if (code == k_ESteamNetConnectionEnd_Remote_Timeout || code == k_ESteamNetConnectionEnd_Misc_Timeout)
				return DisconnectReason::TimedOut;

			if (code == k_ESteamNetConnectionEnd_Remote_BadProtocolVersion)
				return DisconnectReason::ProtocolMismatch;

			return DisconnectReason::Transport;
		}

		/** listen() or connect()'s address, read. */
		struct Address
		{
			bool steam		 = false; // through Steam's relays, rather than UDP
			u64 steam_id	 = 0;	  // steam, connecting: whom to reach
			i32 virtual_port = 0;	  // steam: which of the listener's ports
			SteamNetworkingIPAddr ip; // UDP: host and port
		};

		/** A whole decimal number in [0, max], all of text. */
		template <class T> [[nodiscard]] bool parse_number(StringView text, T max, T& out) noexcept
		{
			T value					 = 0;
			const char* const end	 = text.data() + text.size();
			const auto [stop, error] = std::from_chars(text.data(), end, value);

			if (text.empty() || error != std::errc() || stop != end || value > max)
				return false;

			out = value;
			return true;
		}

		/** "ip:port", "[ipv6]:port" or ":port". Steam builds parse through Steam: only once networking is up. */
		[[nodiscard]] bool parse_ip(StringView text, SteamNetworkingIPAddr& address) noexcept
		{
			address.Clear();

			// A bare port is every interface, IPv4 and IPv6.
			if (text.starts_with(':'))
			{
				u16 port = 0;
				if (!parse_number(text.substr(1), u16{65535}, port))
					return false;

				address.m_port = port;
				return true;
			}

			char buffer[SteamNetworkingIPAddr::k_cchMaxString] = {};
			if (text.empty() || text.size() >= sizeof(buffer))
				return false;

			std::memcpy(buffer, text.data(), text.size());
			return address.ParseString(buffer);
		}

		/**
		 * UDP addresses, or Steam ones: "steam" or "steam:<virtual port>" to listen, "steam:<steam id>" or
		 * "steam:<steam id>:<virtual port>" to connect. False when malformed.
		 */
		[[nodiscard]] bool parse_address(StringView text, bool connecting, Address& out) noexcept
		{
			out = {};

			if (text != "steam" && !text.starts_with("steam:"))
				return parse_ip(text, out.ip);

			out.steam = true;
			if (text == "steam")
				return !connecting; // listening on virtual port 0; connecting needs someone to reach

			StringView rest = text.substr(6);
			if (connecting)
			{
				const size_t colon = rest.find(':');
				if (!parse_number(rest.substr(0, colon), std::numeric_limits<u64>::max(), out.steam_id) ||
					out.steam_id == 0)
					return false;

				if (colon == StringView::npos)
					return true;

				rest = rest.substr(colon + 1);
			}

			u16 port = 0;
			if (!parse_number(rest, u16{65535}, port))
				return false;

			out.virtual_port = port;
			return true;
		}

		/** No host given: "[::]", or ":port" read as one. */
		[[nodiscard]] bool any_host(const SteamNetworkingIPAddr& address) noexcept
		{
			return std::all_of(std::begin(address.m_ipv6), std::end(address.m_ipv6), [](u8 byte) { return byte == 0; });
		}

		void log_output(ESteamNetworkingSocketsDebugOutputType type, const char* message)
		{
			if (type <= k_ESteamNetworkingSocketsDebugOutputType_Error)
				EMBER_ERROR("{}: {}", LIBRARY, message);
			else if (type <= k_ESteamNetworkingSocketsDebugOutputType_Warning)
				EMBER_WARN("{}: {}", LIBRARY, message);
			else
				EMBER_INFO("{}: {}", LIBRARY, message);
		}
	}

	struct ValveTransport::Callbacks
	{
		static void status_changed(SteamNetConnectionStatusChangedCallback_t* info)
		{
			std::scoped_lock routes(s_routes_lock);

			const HSteamNetConnection connection		= info->m_hConn;
			const ESteamNetworkingConnectionState state = info->m_info.m_eState;
			ValveTransport* transport					= find_route(connection, false);

			// A client knocking on a listen socket: accept it into that transport. It reports
			// Connected once the handshake completes.
			if (transport == nullptr && state == k_ESteamNetworkingConnectionState_Connecting &&
				info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid)
			{
				transport = find_route(info->m_info.m_hListenSocket, true);
				if (transport == nullptr)
					return;

				if (sockets().AcceptConnection(connection) != k_EResultOK || !add_route(connection, false, transport))
				{
					sockets().CloseConnection(connection, end_code(DisconnectReason::Transport), "not accepted", false);
					return;
				}

				transport->add_peer(connection, false, true);
				return;
			}

			const bool ended = state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
							   state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally;

			// Not ours, or closed by us already: an ended connection's handle still needs freeing.
			if (transport == nullptr)
			{
				if (ended)
					sockets().CloseConnection(connection, 0, nullptr, false);
				return;
			}

			if (state == k_ESteamNetworkingConnectionState_Connected)
				transport->connected(connection);

			if (ended)
			{
				remove_route(connection, false);
				transport->closed(connection, reason_of(info->m_info.m_eEndReason));
				sockets().CloseConnection(connection, 0, nullptr, false);
			}
		}
	};

	Result<void, TransportError> initialize(const NetDef& def) noexcept
	{
		if (s_claimed.exchange(true))
			return fail(TransportError::AlreadyInitialized);

#if EMBER_USE_STEAM
		// Steam is the app's to start and stop; take the interface for the role it started.
		ISteamNetworkingSockets* const api =
			def.steam_role == SteamRole::Server ? SteamGameServerNetworkingSockets() : SteamNetworkingSockets();

		if (api == nullptr || SteamNetworkingUtils() == nullptr)
		{
			EMBER_ERROR("Steam networking is unavailable: start Steam as a {} first", enum_name(def.steam_role));
			s_claimed = false;
			return fail(NetError::SteamNotRunning);
		}

		// Measuring the relays takes a few seconds: start now, not at the first connection to a friend.
		if (def.steam_role == SteamRole::Client)
			SteamNetworkingUtils()->InitRelayNetworkAccess();
#else
		(void)def;
		SteamNetworkingSockets_SetServiceThreadInitCallback([] { set_thread_name("gns service"); });

		SteamNetworkingErrMsg message = {};
		if (!GameNetworkingSockets_Init(nullptr, message))
		{
			EMBER_ERROR("GameNetworkingSockets failed to start: {}", message);
			s_claimed = false;
			return fail(TransportError::LibraryFailed);
		}

		ISteamNetworkingSockets* const api = SteamNetworkingSockets();
#endif

		ISteamNetworkingUtils& utils = *SteamNetworkingUtils();
		utils.SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, &log_output);
		utils.SetGlobalCallback_SteamNetConnectionStatusChanged(&ValveTransport::Callbacks::status_changed);

		s_sockets.store(api, std::memory_order_release);
		return {};
	}

	void shutdown() noexcept
	{
		if (s_sockets.exchange(nullptr) == nullptr)
			return;

		EMBER_ASSERT(s_route_count == 0 && "destroy every ValveTransport before net::shutdown()");

		ISteamNetworkingUtils& utils = *SteamNetworkingUtils();
		utils.SetGlobalCallback_SteamNetConnectionStatusChanged(nullptr);
		utils.SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);

#if !EMBER_USE_STEAM
		GameNetworkingSockets_Kill();
#endif

		s_claimed = false;
	}

	bool is_initialized() noexcept { return s_sockets.load(std::memory_order_acquire) != nullptr; }

	void simulate(const LinkConditions& conditions) noexcept
	{
		if (!is_initialized())
			return;

		const auto ms	   = [](f64 seconds) { return static_cast<int32>(std::lround(seconds * 1000.0)); };
		const int32 jitter = ms(conditions.jitter);

		ISteamNetworkingUtils& utils = *SteamNetworkingUtils();
		utils.SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, ms(conditions.latency));
		utils.SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, conditions.loss * 100.0f);
		utils.SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, jitter * 0.5f);
		utils.SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, static_cast<f32>(jitter));
		utils.SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, jitter > 0 ? 100.0f : 0.0f);
		utils.SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Send, conditions.duplicate * 100.0f);
		utils.SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketDup_TimeMax, jitter);
		utils.SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Send,
										conditions.reorder ? 10.0f : 0.0f);
		utils.SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketReorder_Time, std::max(jitter, 1));
	}

	ValveTransport::ValveTransport() noexcept
		: m_peers(&memory::heap(MemoryTag::Network)), m_status(&memory::heap(MemoryTag::Network))
	{
		if (is_initialized())
			m_poll_group = sockets().CreatePollGroup();
	}

	ValveTransport::~ValveTransport() noexcept
	{
		if (!ready())
			return;

		std::scoped_lock routes(s_routes_lock);
		remove_routes(this);

		// Unrouted, so the far ends' Disconnected comes from the library and nothing comes back here.
		for (const Peer& peer : m_peers)
			sockets().CloseConnection(peer.connection, end_code(DisconnectReason::Shutdown), "shutdown", false);

		if (m_listen_socket != k_HSteamListenSocket_Invalid)
			sockets().CloseListenSocket(m_listen_socket);

		sockets().DestroyPollGroup(m_poll_group);
	}

	Result<void, TransportError> ValveTransport::listen(StringView address) noexcept
	{
		if (!ready())
			return fail(TransportError::NotInitialized);

		if (m_listen_socket != k_HSteamListenSocket_Invalid)
			return fail(TransportError::AlreadyListening);

		Address where;
		if (!parse_address(address, false, where) || (!where.steam && where.ip.m_port == 0))
			return fail(TransportError::BadAddress);

		if (where.steam && !STEAM_NETWORKING)
			return fail(TransportError::Unsupported);

		std::scoped_lock routes(s_routes_lock);

		const HSteamListenSocket socket = where.steam ? sockets().CreateListenSocketP2P(where.virtual_port, 0, nullptr)
													  : sockets().CreateListenSocketIP(where.ip, 0, nullptr);
		if (socket == k_HSteamListenSocket_Invalid)
			return fail(TransportError::AddressInUse);

		if (!add_route(socket, true, this))
		{
			sockets().CloseListenSocket(socket);
			return fail(TransportError::AddressInUse);
		}

		m_listen_socket = socket;
		return {};
	}

	Result<PeerId, TransportError> ValveTransport::connect(StringView address, f64 /*now*/) noexcept
	{
		if (!ready())
			return fail(TransportError::NotInitialized);

		Address where;
		if (!parse_address(address, true, where) || (!where.steam && (where.ip.m_port == 0 || any_host(where.ip))))
			return fail(TransportError::BadAddress);

		if (where.steam && !STEAM_NETWORKING)
			return fail(TransportError::Unsupported);

		std::scoped_lock routes(s_routes_lock);

		HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
		if (where.steam)
		{
			SteamNetworkingIdentity identity;
			identity.Clear();
			identity.SetSteamID64(where.steam_id);
			connection = sockets().ConnectP2P(identity, where.virtual_port, 0, nullptr);
		}
		else
		{
			connection = sockets().ConnectByIPAddress(where.ip, 0, nullptr);
		}

		if (connection == k_HSteamNetConnection_Invalid)
			return fail(TransportError::Unreachable);

		if (!add_route(connection, false, this))
		{
			sockets().CloseConnection(connection, end_code(DisconnectReason::Transport), "no route", false);
			return fail(TransportError::Unreachable);
		}

		return add_peer(connection, false, false);
	}

	Result<PeerId, TransportError> ValveTransport::connect_local(ValveTransport& server, bool through_network,
																 f64 /*now*/) noexcept
	{
		if (!ready() || !server.ready())
			return fail(TransportError::NotInitialized);

		std::scoped_lock routes(s_routes_lock);

		HSteamNetConnection mine   = k_HSteamNetConnection_Invalid;
		HSteamNetConnection theirs = k_HSteamNetConnection_Invalid;

		if (!sockets().CreateSocketPair(&mine, &theirs, through_network, nullptr, nullptr))
			return fail(TransportError::Unreachable);

		if (!add_route(mine, false, this) || !add_route(theirs, false, &server))
		{
			remove_route(mine, false);
			sockets().CloseConnection(mine, 0, nullptr, false);
			sockets().CloseConnection(theirs, 0, nullptr, false);
			return fail(TransportError::Unreachable);
		}

		// A pair starts connected and posts no callback for it, so both ends are told here.
		server.add_peer(theirs, true, true);
		return add_peer(mine, true, false);
	}

	void ValveTransport::disconnect(PeerId peer, DisconnectReason reason, f64 /*now*/) noexcept
	{
		u32 connection = k_HSteamNetConnection_Invalid;

		{
			std::scoped_lock lock(m_lock);

			for (u32 i = 0; i < m_peers.size(); ++i)
			{
				if (m_peers[i].id == peer)
				{
					connection = m_peers[i].connection;
					m_peers.erase(m_peers.begin() + i);
					break;
				}
			}

			// Whatever was still waiting to be said about the peer is moot now.
			std::erase_if(m_status, [peer](const Status& status) { return status.peer == peer; });
		}

		if (connection == k_HSteamNetConnection_Invalid)
			return;

		std::scoped_lock routes(s_routes_lock);
		remove_route(connection, false);
		sockets().CloseConnection(connection, end_code(reason), enum_name(reason), false);
	}

	void ValveTransport::send(PeerId peer, Span<const u8> data, Delivery delivery, f64 /*now*/) noexcept
	{
		EMBER_ASSERT(data.size() <= MAX_PACKET_BYTES && "larger than a transport carries in one piece");

		const u32 connection = connection_of(peer);
		if (connection == k_HSteamNetConnection_Invalid || data.size() > MAX_PACKET_BYTES)
			return;

		const int flags = delivery == Delivery::Reliable ? k_nSteamNetworkingSend_ReliableNoNagle
														 : k_nSteamNetworkingSend_UnreliableNoNagle;

		sockets().SendMessageToConnection(connection, data.data(), static_cast<uint32>(data.size()), flags, nullptr);
	}

	bool ValveTransport::poll(f64 now, TransportEvent& event) noexcept
	{
		if (!ready())
			return false;

		// Connection changes for every transport in the process, each queued on its own transport.
		sockets().RunCallbacks();

		{
			std::scoped_lock lock(m_lock);

			if (!m_status.empty())
			{
				const Status status = m_status.front();
				m_status.erase(m_status.begin());

				event = {.kind = status.kind, .peer = status.peer, .time = now, .reason = status.reason};
				return true;
			}
		}

		SteamNetworkingMessage_t* message = nullptr;

		while (sockets().ReceiveMessagesOnPollGroup(m_poll_group, &message, 1) == 1)
		{
			const PeerId peer	= static_cast<PeerId>(message->m_nConnUserData);
			const u32 size		= message->m_cbSize > 0 ? static_cast<u32>(message->m_cbSize) : 0;
			const bool reliable = (message->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0;

			// Arrival is stamped on the library's receive thread; carry the message's age over to the caller's clock.
			const SteamNetworkingMicroseconds age =
				SteamNetworkingUtils()->GetLocalTimestamp() - message->m_usecTimeReceived;

			const bool ours = peer != NO_PEER && size <= MAX_PACKET_BYTES;
			if (ours)
				std::memcpy(m_received.data(), message->m_pData, size);

			message->Release();

			if (!ours)
				continue; // more than any packet: not from this protocol

			event = {
				.kind	  = TransportEventKind::Received,
				.peer	  = peer,
				.time	  = now - static_cast<f64>(std::max<SteamNetworkingMicroseconds>(age, 0)) * 1e-6,
				.data	  = Span<const u8>(m_received.data(), size),
				.delivery = reliable ? Delivery::Reliable : Delivery::Unreliable,
			};

			return true;
		}

		return false;
	}

	PeerStats ValveTransport::stats(PeerId peer) const noexcept
	{
		const u32 connection = connection_of(peer);
		if (connection == k_HSteamNetConnection_Invalid)
			return {};

		SteamNetConnectionRealTimeStatus_t status = {};
		if (sockets().GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK)
			return {};

		return {
			.ping				= static_cast<f32>(status.m_nPing) * 0.001f,
			.quality_local		= std::max(status.m_flConnectionQualityLocal, 0.0f),
			.quality_remote		= std::max(status.m_flConnectionQualityRemote, 0.0f),
			.send_rate			= static_cast<f32>(status.m_nSendRateBytesPerSecond),
			.bytes_out			= status.m_flOutBytesPerSec,
			.bytes_in			= status.m_flInBytesPerSec,
			.queue_time			= static_cast<f32>(status.m_usecQueueTime) * 1e-6f,
			.pending_unreliable = static_cast<u32>(std::max(status.m_cbPendingUnreliable, 0)),
			.pending_reliable	= static_cast<u32>(std::max(status.m_cbPendingReliable, 0)),
		};
	}

	u64 ValveTransport::steam_id(PeerId peer) const noexcept
	{
		const u32 connection = connection_of(peer);
		if (connection == k_HSteamNetConnection_Invalid)
			return 0;

		SteamNetConnectionInfo_t info = {};
		if (!sockets().GetConnectionInfo(connection, &info))
			return 0;

		return info.m_identityRemote.GetSteamID64();
	}

	u16 ValveTransport::listen_port() const noexcept
	{
		if (!ready() || m_listen_socket == k_HSteamListenSocket_Invalid)
			return 0;

		SteamNetworkingIPAddr address;
		return sockets().GetListenSocketAddress(m_listen_socket, &address) ? address.m_port : 0;
	}

	u32 ValveTransport::connection_count() const noexcept
	{
		std::scoped_lock lock(m_lock);
		return static_cast<u32>(m_peers.size());
	}

	bool ValveTransport::ready() const noexcept
	{
		return is_initialized() && m_poll_group != k_HSteamNetPollGroup_Invalid;
	}

	u32 ValveTransport::connection_of(PeerId peer) const noexcept
	{
		std::scoped_lock lock(m_lock);

		for (const Peer& entry : m_peers)
		{
			if (entry.id == peer)
				return entry.connection;
		}

		return k_HSteamNetConnection_Invalid;
	}

	PeerId ValveTransport::add_peer(u32 connection, bool connected, bool incoming) noexcept
	{
		// Callers hold the route lock, so no callback for this connection can run before the peer exists.
		PeerId id = NO_PEER;
		{
			std::scoped_lock lock(m_lock);
			id = m_next_peer++;
		}

		sockets().SetConnectionUserData(connection, static_cast<int64>(id));
		sockets().SetConnectionPollGroup(connection, m_poll_group);

		std::scoped_lock lock(m_lock);
		m_peers.push_back({.id = id, .connection = connection, .connected = connected, .incoming = incoming});

		if (connected)
			m_status.push_back({.kind = TransportEventKind::Connected, .peer = id});

		return id;
	}

	void ValveTransport::connected(u32 connection) noexcept
	{
		std::scoped_lock lock(m_lock);

		for (Peer& peer : m_peers)
		{
			if (peer.connection == connection && !peer.connected)
			{
				peer.connected = true;
				m_status.push_back({.kind = TransportEventKind::Connected, .peer = peer.id});
			}
		}
	}

	void ValveTransport::closed(u32 connection, DisconnectReason reason) noexcept
	{
		std::scoped_lock lock(m_lock);

		for (u32 i = 0; i < m_peers.size(); ++i)
		{
			if (m_peers[i].connection != connection)
				continue;

			// A client that gave up before its handshake finished was never announced, so it goes
			// quietly. A connection this end asked for reports how it failed.
			if (m_peers[i].connected || !m_peers[i].incoming)
				m_status.push_back({.kind = TransportEventKind::Disconnected, .peer = m_peers[i].id, .reason = reason});

			m_peers.erase(m_peers.begin() + i);
			return;
		}
	}
}
