#pragma once

#include <ember/net/command_stream.h>
#include <ember/net/connection.h>
#include <ember/net/messages.h>
#include <ember/net/replica.h>
#include <ember/net/time_dilation.h>
#include <ember/net/transport.h>

namespace ember::net
{
	struct ClientDef
	{
		CommandCodec commands	 = {}; // the game's command type: command_codec<T>()
		u32 game_protocol		 = 0;  // the game's wire version: a server with another refuses it
		f64 tick_seconds		 = 1.0 / 60.0;
		f64 handshake_timeout	 = 5.0;		// seconds from connect() to the Welcome
		ConnectionDef connection = {};		// the silence after which the server counts as gone
		TimeDilationDef dilation = {};		// how the clock steers toward the server
		Replica* replica		 = nullptr; // puts the server's entities in the client world; null ignores them
	};

	enum class ClientState : u8
	{
		Idle,		// not connected, or the connection ended
		Connecting, // the transport is connecting
		Greeting,	// connected, Hello sent, waiting for the Welcome
		Playing,	// welcomed: commands  go out every tick
		Count,
	};

	enum class ClientEventKind : u8
	{
		Joined,	 // welcomed: start the tick clock at `tick` and send a command every tick
		Left,	 // the connection is over: refused, kicked, timed out, or the server went away
		Message, // the server's game message, delivered once and in order
		Count
	};

	struct ClientEvent
	{
		ClientEventKind kind	= ClientEventKind::Message;
		u8 slot					= 0;					  // Joined: the seat the server gave
		Tick tick				= NO_TICK;				  // Joined: the tick to start the clock at
		DisconnectReason reason = DisconnectReason::None; // Left: why
		Span<const u8> data		= {};					  // Message: the bytes, valid until the next poll()
	};

	/**
	 * The client's end of the connection to a server: the handshake, the command stream, the timing reports,
	 * the clock they steer, the game's reliable messages, and noticing when the server goes quiet.
	 *
	 * connect() starts the transport; once it is up the client says Hello, and the server answers Welcome with
	 * a seat and its clock, or closes the connection with the reason it refused. The round trip of that exchange,
	 * less the time the server held the Hello, tells the client how far ahead of the server to start its own
	 * clock: Joined carries that tick.
	 *
	 * From then on the game sends one command per tick it simulates, and each send carries every command the
	 * server has not acknowledged. Each frame the game polls, then sets its tick clock's rate from time_scale()
	 * and moves its tick by take_jump(): the time dilation that keeps commands arriving just before the server
	 * needs them.
	 *
	 * Single threaded: the client's game thread drives it. The transport must outlive it.
	 */
	class Client final
	{
	public:
		Client(Transport& transport, const ClientDef& def) noexcept;

		/** Leaves the server, if connected: it sees Left (Shutdown). */
		~Client() noexcept;

		Client(const Client&)			 = delete;
		Client& operator=(const Client&) = delete;

		/** Connects to a server at an address the transport understands. Joined or Left follows from poll(). */
		[[nodiscard]] Result<void, TransportError> connect(StringView address, f64 now) noexcept;

		/**
		 * Uses a connection the transport has already made, as ValveTransport::connect_local does for single
		 * player. The handshake runs after connect().
		 */
		void attach(PeerId peer, f64 now) noexcept;

		/** Leaves the server; it sees Left with the reason. No Left event follows here. */
		void disconnect(DisconnectReason reason, f64 now) noexcept;

		/** The next event; false once there is none. Reads every packet and message; call it every frame. */
		[[nodiscard]] bool poll(f64 now, ClientEvent& event) noexcept;

		/**
		 * Records the command for tick, which the client has just simulated, and sends the packet that carries
		 * it with every command the server still lacks. view_delay is how many ticks behind the remote entities
		 * on screen were. A tick simulated again after the clock jumped back keeps the command first recorded
		 * for it. Ignored until Playing.
		 */
		template <class T> void send_command(Tick tick, const T& command, f32 view_delay, f64 now) noexcept
		{
			if (m_state != ClientState::Playing)
				return;

			m_commands.push(tick, command, view_delay);
			send_packet(now);
		}

		/**
		 * The command recorded for tick; false if there is none. After the clock jumps back, the game replays
		 * these for the ticks it runs again.
		 */
		template <class T> [[nodiscard]] bool find_command(Tick tick, T& out) const noexcept
		{
			return m_commands.find(tick, out);
		}

		/** A game message for the server: reliable, in order. At most MAX_MESSAGE_BYTES. Ignored until Playing. */
		void send_message(Span<const u8> data, f64 now) noexcept;

		ClientState state() const noexcept { return m_state; }

		/** The seat the server gave; meaningful once Playing. */
		u8 slot() const noexcept { return m_slot; }

		/** The rate for the game's tick clock: 1 is real time. */
		f32 time_scale() const noexcept { return m_dilation.time_scale(); }

		/** Ticks to move the game's tick clock by now, positive forward; reading clears it. */
		i32 take_jump() noexcept { return m_dilation.take_jump(); }

		/** The clock's steering: its aim, its error and its epoch. */
		const TimeDilation& dilation() const noexcept { return m_dilation; }

		/** The connections statstics: round trip, loss, rates. */
		const ConnectionStats& stats() const noexcept { return m_connection.stats(); }

		/** The transport's peer for the server, NO_PEER when not connected. */
		PeerId peer() const noexcept { return m_peer; }

	private:
		void send_packet(f64 now) noexcept;
		void received_packet(const TransportEvent& incoming) noexcept;
		[[nodiscard]] bool received_message(const TransportEvent& incoming, f64 now, ClientEvent& event) noexcept;

		/** Ends the session from this end, closing the connection if it is still open, and reports Left. */
		[[nodiscard]] bool leave(DisconnectReason reason, f64 now, ClientEvent& event) noexcept;

		Transport& m_transport;
		ClientDef m_def;

		ClientState m_state = ClientState::Idle;
		PeerId m_peer		= NO_PEER;
		u8 m_slot			= 0;

		f64 m_connect_time = 0.0; // when connect() was called, for the handshake timeout
		f64 m_hello_time   = 0.0; // when the Hello left, for the first round trip
		f64 m_now		   = 0.0; // the latest time the game gave, for the destructor's goodbye

		Connection m_connection;
		CommandSender m_commands;
		TimeDilation m_dilation;

		/** The message() poll handed out last; the event's span points into it until the next poll(). */
		Message m_message;
	};
}

namespace ember
{
	EMBER_ENUM_NAMES(net::ClientState, "Idle", "Connecting", "Greeting", "Playing");
	EMBER_ENUM_NAMES(net::ClientEventKind, "Joined", "Left", "Message");
}
