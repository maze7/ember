#include <ember/memory/memory.h>
#include <ember/net/loopback.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace ember::net
{
	namespace
	{
		// SplitMix64: good enough dice for a simulated network, and one line to seed.
		[[nodiscard]] u64 split_mix(u64& state) noexcept
		{
			u64 z = (state += 0x9e3779b97f4a7c15ull);
			z	  = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
			z	  = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
			return z ^ (z >> 31);
		}
	}

	LoopbackNetwork::LoopbackNetwork(u64 seed, MemoryTag tag) noexcept
		: m_tag(tag), m_random(seed), m_payloads(&memory::heap(tag)), m_free_payloads(&memory::heap(tag)),
		  m_transports(&memory::heap(tag))
	{
	}

	LoopbackNetwork::~LoopbackNetwork() noexcept
	{
		EMBER_ASSERT(m_transports.empty() && "destroy every LoopbackTransport before its network");
	}

	u32 LoopbackNetwork::in_flight() const noexcept
	{
		std::scoped_lock lock(m_lock);

		u32 count = 0;
		for (const LoopbackTransport* transport : m_transports)
			count += static_cast<u32>(transport->m_inbox.size());

		return count;
	}

	f64 LoopbackNetwork::random() noexcept
	{
		// The top 53 bits, as a double in [0, 1).
		return static_cast<f64>(split_mix(m_random) >> 11) * 0x1.0p-53;
	}

	void LoopbackNetwork::schedule(LoopbackTransport& to, Pending pending) noexcept
	{
		pending.order = m_order++;

		// Latest first, so the next one due leaves from the back without moving the rest.
		const auto later = [](const Pending& a, const Pending& b)
		{ return a.time > b.time || (a.time == b.time && a.order > b.order); };

		Vector<Pending>& inbox = to.m_inbox;
		inbox.insert(std::upper_bound(inbox.begin(), inbox.end(), pending, later), pending);
	}

	u32 LoopbackNetwork::store(Span<const u8> bytes) noexcept
	{
		EMBER_ASSERT(bytes.size() <= MAX_PACKET_BYTES);

		u32 slot = 0;
		if (!m_free_payloads.empty())
		{
			slot = m_free_payloads.back();
			m_free_payloads.pop_back();
		}
		else
		{
			slot = static_cast<u32>(m_payloads.size());
			m_payloads.emplace_back();
		}

		std::memcpy(m_payloads[slot].data(), bytes.data(), bytes.size());
		return slot;
	}

	void LoopbackNetwork::release(u32 payload) noexcept { m_free_payloads.push_back(payload); }

	LoopbackTransport::LoopbackTransport(LoopbackNetwork& network) noexcept
		: m_network(network), m_address(&memory::heap(network.m_tag)), m_links(&memory::heap(network.m_tag)),
		  m_inbox(&memory::heap(network.m_tag))
	{
		std::scoped_lock lock(m_network.m_lock);
		m_network.m_transports.push_back(this);
	}

	LoopbackTransport::~LoopbackTransport() noexcept
	{
		std::scoped_lock lock(m_network.m_lock);

		// Every link that leads here dies now. The far ends hear about it at once, lose whatever this
		// end still had on the way to them, and stop sending into a transport that is gone.
		for (LoopbackTransport* other : m_network.m_transports)
		{
			for (Link& link : other->m_links)
			{
				if (link.other != this)
					continue;

				std::erase_if(other->m_inbox,
							  [&](const LoopbackNetwork::Pending& pending)
							  {
								  const bool from_here =
									  pending.peer == link.peer && pending.kind == TransportEventKind::Received;

								  if (from_here)
									  m_network.release(pending.payload);

								  return from_here;
							  });

				link.other = nullptr;
				m_network.schedule(*other, {.time	= 0.0,
											.peer	= link.peer,
											.kind	= TransportEventKind::Disconnected,
											.reason = DisconnectReason::Shutdown});
			}
		}

		for (const LoopbackNetwork::Pending& pending : m_inbox)
		{
			if (pending.kind == TransportEventKind::Received)
				m_network.release(pending.payload);
		}

		std::erase(m_network.m_transports, this);
	}

	void LoopbackTransport::set_conditions(const LinkConditions& conditions) noexcept
	{
		EMBER_ASSERT(conditions.latency >= 0.0 && conditions.jitter >= 0.0);
		EMBER_ASSERT(conditions.loss >= 0.0f && conditions.loss <= 1.0f);
		EMBER_ASSERT(conditions.duplicate >= 0.0f && conditions.duplicate <= 1.0f);

		std::scoped_lock lock(m_network.m_lock);
		m_conditions = conditions;
	}

	Result<void, TransportError> LoopbackTransport::listen(StringView address) noexcept
	{
		std::scoped_lock lock(m_network.m_lock);

		if (address.empty())
			return fail(TransportError::BadAddress);

		if (!m_address.empty())
			return fail(TransportError::AlreadyListening);

		for (const LoopbackTransport* other : m_network.m_transports)
		{
			if (other->m_address == address)
				return fail(TransportError::AddressInUse);
		}

		m_address = address;
		return {};
	}

	Result<PeerId, TransportError> LoopbackTransport::connect(StringView address, f64 now) noexcept
	{
		std::scoped_lock lock(m_network.m_lock);

		if (address.empty())
			return fail(TransportError::BadAddress);

		LoopbackTransport* server = nullptr;
		for (LoopbackTransport* other : m_network.m_transports)
		{
			if (other->m_address == address)
				server = other;
		}

		if (server == nullptr)
			return fail(TransportError::Unreachable);

		const PeerId mine	= m_next_peer++;
		const PeerId theirs = server->m_next_peer++;

		m_links.push_back({.peer = mine, .other = server, .other_peer = theirs});
		server->m_links.push_back({.peer = theirs, .other = this, .other_peer = mine});

		// The server hears of the connection one way in; the client one round trip after asking.
		const f64 there = now + m_conditions.latency;
		const f64 back	= there + server->m_conditions.latency;

		m_network.schedule(*server, {.time = there, .peer = theirs, .kind = TransportEventKind::Connected});
		m_network.schedule(*this, {.time = back, .peer = mine, .kind = TransportEventKind::Connected});

		return mine;
	}

	void LoopbackTransport::disconnect(PeerId peer, DisconnectReason reason, f64 now) noexcept
	{
		std::scoped_lock lock(m_network.m_lock);

		Link* link = find_link(peer);
		if (link == nullptr)
			return;

		// Behind everything already on its way, as a close on a real path would be.
		if (link->other != nullptr)
		{
			const f64 time = std::max(arrival_time(*link, Delivery::Reliable, now), link->last_arrival);
			m_network.schedule(
				*link->other,
				{.time = time, .peer = link->other_peer, .kind = TransportEventKind::Disconnected, .reason = reason});
		}

		remove_link(peer);
	}

	void LoopbackTransport::send(PeerId peer, Span<const u8> data, Delivery delivery, f64 now) noexcept
	{
		EMBER_ASSERT(data.size() <= MAX_PACKET_BYTES && "larger than a transport carries in one piece");

		if (data.size() > MAX_PACKET_BYTES)
			return;

		std::scoped_lock lock(m_network.m_lock);

		Link* link = find_link(peer);
		if (link == nullptr || link->other == nullptr)
			return;

		// Reliable sends always arrive, once; unreliable ones take their chances.
		u32 copies = 1;
		if (delivery == Delivery::Unreliable)
		{
			copies = (m_network.random() < m_conditions.loss ? 0u : 1u) +
					 (m_network.random() < m_conditions.duplicate ? 1u : 0u);
		}

		for (u32 i = 0; i < copies; ++i)
		{
			m_network.schedule(*link->other, {.time		= arrival_time(*link, delivery, now),
											  .peer		= link->other_peer,
											  .kind		= TransportEventKind::Received,
											  .delivery = delivery,
											  .payload	= m_network.store(data),
											  .size		= static_cast<u32>(data.size())});
		}
	}

	bool LoopbackTransport::poll(f64 now, TransportEvent& event) noexcept
	{
		std::scoped_lock lock(m_network.m_lock);

		while (!m_inbox.empty() && m_inbox.back().time <= now)
		{
			const LoopbackNetwork::Pending pending = m_inbox.back();
			m_inbox.pop_back();

			// A connection this end has closed hears nothing more.
			const bool open = find_link(pending.peer) != nullptr;

			if (pending.kind == TransportEventKind::Received)
			{
				if (open)
					std::memcpy(m_received.data(), m_network.m_payloads[pending.payload].data(), pending.size);

				m_network.release(pending.payload);
			}

			if (!open)
				continue;

			if (pending.kind == TransportEventKind::Disconnected)
				remove_link(pending.peer);

			event = {
				.kind = pending.kind,
				.peer = pending.peer,
				.time = pending.time,
				.data = pending.kind == TransportEventKind::Received ? Span<const u8>(m_received.data(), pending.size)
																	 : Span<const u8>(),
				.delivery = pending.delivery,
				.reason	  = pending.reason,
			};

			return true;
		}

		return false;
	}

	PeerStats LoopbackTransport::stats(PeerId peer) const noexcept
	{
		std::scoped_lock lock(m_network.m_lock);

		const Link* link = find_link(peer);
		if (link == nullptr || link->other == nullptr)
			return {};

		// The configured weather is all the loopback knows; it never queues.
		const f64 round_trip = m_conditions.latency + link->other->m_conditions.latency;
		return {.ping			= static_cast<f32>(round_trip),
				.quality_local	= 1.0f - link->other->m_conditions.loss,
				.quality_remote = 1.0f - m_conditions.loss};
	}

	u32 LoopbackTransport::connection_count() const noexcept
	{
		std::scoped_lock lock(m_network.m_lock);
		return static_cast<u32>(m_links.size());
	}

	LoopbackTransport::Link* LoopbackTransport::find_link(PeerId peer) noexcept
	{
		for (Link& link : m_links)
		{
			if (link.peer == peer)
				return &link;
		}

		return nullptr;
	}

	const LoopbackTransport::Link* LoopbackTransport::find_link(PeerId peer) const noexcept
	{
		return const_cast<LoopbackTransport*>(this)->find_link(peer);
	}

	void LoopbackTransport::remove_link(PeerId peer) noexcept
	{
		std::erase_if(m_links, [peer](const Link& link) { return link.peer == peer; });
	}

	f64 LoopbackTransport::arrival_time(Link& link, Delivery delivery, f64 now) noexcept
	{
		f64 time = now + m_conditions.latency + m_conditions.jitter * m_network.random();

		if (delivery == Delivery::Reliable)
		{
			time			   = std::max(time, link.last_reliable);
			link.last_reliable = time;
			return time;
		}

		if (!m_conditions.reorder)
			time = std::max(time, link.last_arrival);

		link.last_arrival = std::max(link.last_arrival, time);
		return time;
	}
}
