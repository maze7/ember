#pragma once

#include <ember/memory/common.h>
#include <ember/net/transport.h>
#include <ember/sync/spin_mutex.h>

#include <array>

namespace ember::net
{
	class LoopbackTransport;

	/**
	 * A simulated wire between LoopbackTransports in one process, on the caller's clock: what the
	 * netcode's own tests run on, and tools that want exact, repeatable network weather (a soak
	 * test of the clock, a bot harness). Games connect through Valve's networking sockets
	 * (valve_transport.h), in process too.
	 *
	 * Unreliable datagrams meet each sender's LinkConditions; reliable ones are never lost or
	 * repeated and arrive in the order sent, after the latency and jitter, as a transport that
	 * resends would deliver them. The dice come from one seeded generator, so a test that drives
	 * both ends from one thread replays exactly. Datagrams are copied, so the ends may live on
	 * different threads; one lock guards every queue.
	 *
	 * Transports register on construction and must all be destroyed before the network is.
	 */
	class LoopbackNetwork final
	{
	public:
		explicit LoopbackNetwork(u64 seed = 1, MemoryTag tag = MemoryTag::Network) noexcept;
		~LoopbackNetwork() noexcept;

		LoopbackNetwork(const LoopbackNetwork&)			   = delete;
		LoopbackNetwork& operator=(const LoopbackNetwork&) = delete;

		/** Events scheduled but not yet polled, everywhere. For tests and debug views. */
		[[nodiscard]] u32 in_flight() const noexcept;

	private:
		friend class LoopbackTransport;

		using Payload = std::array<u8, MAX_PACKET_BYTES>;

		struct Pending
		{
			f64 time				= 0.0;
			u64 order				= 0;	   // breaks ties first in, first out
			PeerId peer				= NO_PEER; // the receiver's id for the sender
			TransportEventKind kind = TransportEventKind::Received;
			Delivery delivery		= Delivery::Unreliable;
			DisconnectReason reason = DisconnectReason::None;
			u32 payload				= 0; // Received: the slot holding the bytes
			u32 size				= 0;
		};

		// Everything below runs under m_lock.
		[[nodiscard]] f64 random() noexcept;
		void schedule(LoopbackTransport& to, Pending pending) noexcept;
		[[nodiscard]] u32 store(Span<const u8> bytes) noexcept;
		void release(u32 payload) noexcept;

		MemoryTag m_tag;
		mutable SpinMutex m_lock;
		u64 m_random = 0;
		u64 m_order	 = 0;

		Vector<Payload> m_payloads;
		Vector<u32> m_free_payloads;
		Vector<LoopbackTransport*> m_transports;
	};

	/** A Transport whose wire is a LoopbackNetwork. An address is any string a listener claimed. */
	class LoopbackTransport final : public Transport
	{
	public:
		explicit LoopbackTransport(LoopbackNetwork& network) noexcept;

		/** Closes every connection at once, as a crashed process would: peers see Disconnected (Shutdown). */
		~LoopbackTransport() noexcept override;

		/** Conditions for what this end sends from now on. */
		void set_conditions(const LinkConditions& conditions) noexcept;

		[[nodiscard]] Result<void, TransportError> listen(StringView address) noexcept override;
		[[nodiscard]] Result<PeerId, TransportError> connect(StringView address, f64 now) noexcept override;
		void disconnect(PeerId peer, DisconnectReason reason, f64 now) noexcept override;
		void send(PeerId peer, Span<const u8> data, Delivery delivery, f64 now) noexcept override;
		bool poll(f64 now, TransportEvent& event) noexcept override;
		PeerStats stats(PeerId peer) const noexcept override;

		/** Connections open on this end, for tests. */
		[[nodiscard]] u32 connection_count() const noexcept;

	private:
		friend class LoopbackNetwork;

		struct Link
		{
			PeerId peer				 = NO_PEER; // this end's id for the connection
			LoopbackTransport* other = nullptr; // null once the far end is destroyed
			PeerId other_peer		 = NO_PEER; // the far end's id for it
			f64 last_arrival		 = 0.0;		// latest unreliable arrival scheduled, for in-order delivery
			f64 last_reliable		 = 0.0;		// latest reliable arrival scheduled; reliable never reorders
		};

		// Everything below runs under the network's lock.
		[[nodiscard]] Link* find_link(PeerId peer) noexcept;
		[[nodiscard]] const Link* find_link(PeerId peer) const noexcept;
		void remove_link(PeerId peer) noexcept;
		[[nodiscard]] f64 arrival_time(Link& link, Delivery delivery, f64 now) noexcept;

		LoopbackNetwork& m_network;
		LinkConditions m_conditions = {};
		String m_address;
		Vector<Link> m_links;
		PeerId m_next_peer = 1;

		/** Events addressed here, latest first: the next one due is at the back. */
		Vector<LoopbackNetwork::Pending> m_inbox;

		/** The bytes poll() handed out last; the event's span points here until the next poll(). */
		LoopbackNetwork::Payload m_received = {};
	};
}
