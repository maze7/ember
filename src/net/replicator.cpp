#include <ember/memory/memory.h>
#include <ember/net/replicator.h>

#include <algorithm>
#include <limits>

namespace ember::net
{
	namespace
	{
		/** The tick and both counts: what every section costs before its first entry. */
		constexpr u32 SECTION_HEADER_BITS = 32 + 2 * SECTION_COUNT_BITS;

		/** The entity a viewer owns comes before everything else it is owed. */
		constexpr f32 OWNER_PRIORITY = std::numeric_limits<f32>::max();

		/** Copies a component's stored bits into a packet. */
		void copy_bits(serialize::WriteStream& stream, const ComponentBits& wire) noexcept
		{
			serialize::ReadStream reader(wire.bytes.data(), (wire.bits + 7) / 8);
			for (u32 left = wire.bits; left > 0;)
			{
				const int chunk = static_cast<int>(std::min(left, 32u));
				u32 value		= 0;

				[[maybe_unused]] const bool read = reader.SerializeBits(value, chunk);
				EMBER_ASSERT(read);

				stream.SerializeBits(value, chunk);
				left -= static_cast<u32>(chunk);
			}
		}

		/** Drops a queue's consumed head once it is most of the queue, so the queue reuses its storage. */
		template <class T> void compact(Vector<T>& queue, u32& head) noexcept
		{
			if (head == queue.size())
			{
				queue.clear();
				head = 0;
			}
			else if (head >= 256 && head * 2 >= queue.size())
			{
				queue.erase(queue.begin(), queue.begin() + head);
				head = 0;
			}
		}
	}

	Replicator::Replicator(const Schema& schema, const ReplicatorDef& def) noexcept
		: m_schema(schema), m_def(def), m_entities(&memory::heap(MemoryTag::Network)),
		  m_pools(&memory::heap(MemoryTag::Network)), m_viewers(&memory::heap(MemoryTag::Network)),
		  m_free(&memory::heap(MemoryTag::Network)), m_owed(&memory::heap(MemoryTag::Network)),
		  m_removals(&memory::heap(MemoryTag::Network))
	{
		EMBER_ASSERT(def.max_viewers > 0 && def.max_viewers < NO_OWNER);

		const u32 count = schema.max_entities();
		m_entities.resize(count);

		// A pool per component type, empty until entities have them.
		m_pools.reserve(schema.component_count());
		for (u32 id = 0; id < schema.component_count(); ++id)
		{
			Pool& pool = m_pools.emplace_back(Pool{.sparse = Vector<u32>(&memory::heap(MemoryTag::Network)),
												   .dense  = Vector<u32>(&memory::heap(MemoryTag::Network)),
												   .values = Vector<Stored>(&memory::heap(MemoryTag::Network))});
			pool.sparse.resize(count, NONE);
		}

		// Every index is free at first, lowest first.
		m_free.resize(count);
		for (u32 index = 0; index < count; ++index)
			m_free[index] = index;
		m_free_count = count;

		m_viewers.reserve(def.max_viewers);
		for (u32 seat = 0; seat < def.max_viewers; ++seat)
		{
			Viewer& viewer =
				m_viewers.emplace_back(Viewer{.known   = Vector<Known>(&memory::heap(MemoryTag::Network)),
											  .packets = Vector<SentPacket>(&memory::heap(MemoryTag::Network)),
											  .entries = Vector<Sent>(&memory::heap(MemoryTag::Network))});
			viewer.known.resize(count);
		}

		m_owed.reserve(count);
		m_removals.reserve(count);
	}

	NetId Replicator::create(PrefabId prefab, Tick tick, u8 owner, u32 key) noexcept
	{
		EMBER_ASSERT(prefab < m_schema.prefab_count());
		EMBER_ASSERT(tick != NO_TICK);
		EMBER_ASSERT(owner == NO_OWNER || owner < m_viewers.size());

		if (m_free_count == 0)
			return NO_NET_ID;

		const u32 index = m_free[m_free_head];
		m_free_head		= (m_free_head + 1) % static_cast<u32>(m_free.size());
		--m_free_count;

		Entity& entity = m_entities[index];
		EMBER_ASSERT(!entity.used);

		const PrefabInfo& info = m_schema.prefab(prefab);
		entity.all			   = {.latest = tick};
		entity.shared		   = {.latest = tick};
		entity.set_all		   = NO_TICK;
		entity.set_shared	   = NO_TICK;
		entity.components	   = info.components;
		entity.key			   = key;
		entity.prefab		   = prefab;
		entity.owner		   = owner;
		entity.viewers		   = 0;
		entity.alive		   = true;
		entity.used			   = true;

		// The prefab's values, which every client has already: unchanged, they never travel.
		for (ComponentMask left = info.components; left != 0;)
		{
			const ComponentId component = detail::take_lowest(left);
			Stored& stored				= insert(index, component);
			stored.changed				= NO_TICK;
			stored.wire					= m_schema.prefab_value(prefab, component).wire;
		}

		// Every viewer let go of the index's last entity before it was freed.
		for (Viewer& viewer : m_viewers)
			viewer.known[index] = {};

		m_high = std::max(m_high, index + 1);
		++m_alive;
		return NetId::make(index, entity.generation);
	}

	void Replicator::destroy(NetId id) noexcept
	{
		if (!alive(id))
			return;

		const u32 index = id.index();
		Entity& entity	= m_entities[index];
		entity.alive	= false;
		--m_alive;

		// A removal carries no values: they go now.
		for (ComponentMask left = entity.components; left != 0;)
			erase(index, detail::take_lowest(left));
		entity.components = 0;

		for (Viewer& viewer : m_viewers)
		{
			Known& known = viewer.known[index];
			if (known.presence == Presence::Present)
			{
				known.presence	= Presence::Removing;
				known.in_flight = false;
			}
		}

		if (entity.viewers == 0)
			free_index(index);
	}

	void Replicator::add(NetId id, ComponentId component, const void* value, Tick tick) noexcept
	{
		EMBER_ASSERT(component < m_pools.size());
		if (!alive(id) || component >= m_pools.size())
			return;

		const u32 index = id.index();
		Entity& entity	= m_entities[index];
		if ((entity.components & component_bit(component)) != 0)
		{
			set(id, component, value, tick);
			return;
		}

		EMBER_ASSERT(tick != NO_TICK && tick >= entity.all.latest &&
					 "change components during the tick being simulated");

		ComponentBits wire;
		if (!encode(component, value, wire))
			return;

		Stored& stored = insert(index, component);
		stored.changed = tick;
		stored.wire	   = wire;

		// A different set of components is a change of state, for whoever gets this one.
		entity.components |= component_bit(component);
		entity.set_all = tick;
		entity.all.at(tick);
		if ((m_schema.owner_only() & component_bit(component)) == 0)
		{
			entity.set_shared = tick;
			entity.shared.at(tick);
		}
	}

	void Replicator::set(NetId id, ComponentId component, const void* value, Tick tick) noexcept
	{
		EMBER_ASSERT(component < m_pools.size());
		if (!alive(id) || component >= m_pools.size())
			return;

		const u32 index = id.index();
		Entity& entity	= m_entities[index];
		Stored* stored	= find(index, component);
		EMBER_ASSERT(stored != nullptr && "the entity has no such component: add() gives it one");
		if (stored == nullptr)
			return;

		EMBER_ASSERT(tick != NO_TICK && tick >= entity.all.latest &&
					 "change components during the tick being simulated");

		ComponentBits wire;
		if (!encode(component, value, wire))
			return;

		// The writer leaves the bits past the last one clear, so equal values have equal bytes.
		const u32 bytes = (wire.bits + 7) / 8;
		if (wire.bits == stored->wire.bits && std::memcmp(wire.bytes.data(), stored->wire.bytes.data(), bytes) == 0)
			return;

		stored->wire	= wire;
		stored->changed = tick;

		entity.all.at(tick);
		if ((m_schema.owner_only() & component_bit(component)) == 0)
			entity.shared.at(tick);
	}

	void Replicator::remove(NetId id, ComponentId component, Tick tick) noexcept
	{
		EMBER_ASSERT(component < m_pools.size());
		if (!alive(id) || component >= m_pools.size())
			return;

		const u32 index = id.index();
		Entity& entity	= m_entities[index];
		if ((entity.components & component_bit(component)) == 0)
			return;

		EMBER_ASSERT(tick != NO_TICK && tick >= entity.all.latest &&
					 "change components during the tick being simulated");

		erase(index, component);
		entity.components &= ~component_bit(component);
		entity.set_all = tick;
		entity.all.at(tick);
		if ((m_schema.owner_only() & component_bit(component)) == 0)
		{
			entity.set_shared = tick;
			entity.shared.at(tick);
		}
	}

	bool Replicator::get(NetId id, ComponentId component, void* out) const noexcept
	{
		if (!alive(id) || component >= m_pools.size())
			return false;

		const Stored* stored = find(id.index(), component);
		if (stored == nullptr)
			return false;

		serialize::ReadStream reader(stored->wire.bytes.data(), (stored->wire.bits + 7) / 8);
		return m_schema.component(component).read(reader, out);
	}

	ComponentMask Replicator::components(NetId id) const noexcept
	{
		return alive(id) ? m_entities[id.index()].components : 0;
	}

	bool Replicator::alive(NetId id) const noexcept
	{
		const u32 index = id.index();
		return id && index < m_entities.size() && m_entities[index].alive &&
			   m_entities[index].generation == id.generation();
	}

	void Replicator::set_relevance(u8 viewer, NetId id, f32 relevance) noexcept
	{
		EMBER_ASSERT(viewer < m_viewers.size());
		EMBER_ASSERT(relevance >= 0.0f);

		if (alive(id) && viewer < m_viewers.size())
			m_viewers[viewer].known[id.index()].relevance = relevance;
	}

	void Replicator::add_viewer(u8 seat) noexcept
	{
		EMBER_ASSERT(seat < m_viewers.size());

		remove_viewer(seat);
		m_viewers[seat].active = true;
	}

	void Replicator::remove_viewer(u8 seat) noexcept
	{
		EMBER_ASSERT(seat < m_viewers.size());

		Viewer& viewer = m_viewers[seat];
		for (u32 index = 0; index < m_high; ++index)
		{
			// Whoever takes the seat next must not inherit what this client controlled.
			if (m_entities[index].owner == seat)
				m_entities[index].owner = NO_OWNER;

			Known& known = viewer.known[index];
			if (known.presence != Presence::Absent)
			{
				known.presence = Presence::Absent;
				release(index);
			}

			known = {};
		}

		viewer.active = false;
		viewer.packets.clear();
		viewer.entries.clear();
		viewer.packets_head = 0;
		viewer.entries_head = 0;
	}

	void Replicator::write(u8 seat, serialize::WriteStream& stream, Sequence sequence, Tick tick) noexcept
	{
		EMBER_ASSERT(seat < m_viewers.size() && m_viewers[seat].active);
		Viewer& viewer = m_viewers[seat];

		const u32 used = static_cast<u32>(stream.GetBitsProcessed());
		EMBER_ASSERT(used + SECTION_HEADER_BITS <= MAX_PACKET_BYTES * 8 && "no room left for the entity section");

		// The packet's room, and the budget within it.
		u32 room   = MAX_PACKET_BYTES * 8 - std::min(used + SECTION_HEADER_BITS, MAX_PACKET_BYTES * 8);
		u32 budget = std::min(room, m_def.max_bits > SECTION_HEADER_BITS ? m_def.max_bits - SECTION_HEADER_BITS : 0);

		// What the viewer is owed: removals it has not been sent, and entities it lacks or that changed
		// since it last got them.
		m_owed.clear();
		m_removals.clear();

		for (u32 index = 0; index < m_high; ++index)
		{
			Known& known		 = viewer.known[index];
			const Entity& entity = m_entities[index];

			if (known.presence == Presence::Removing)
			{
				if (!known.in_flight)
					m_removals.push_back(index);
				continue;
			}

			if (!entity.alive)
				continue;

			const bool owner = entity.owner == seat;
			if (known.relevance <= 0.0f && !owner)
			{
				// Out of the viewer's world: what it has of it goes.
				if (known.presence == Presence::Present)
				{
					known.presence	= Presence::Removing;
					known.in_flight = false;
					m_removals.push_back(index);
				}
				continue;
			}

			// A record on its way covers every change made before it was written.
			const bool owed = owner || known.presence == Presence::Absent ||
							  entity.shared.latest > (known.in_flight ? known.sent_tick : known.acked);
			if (!owed)
				continue;

			const f32 rate = m_schema.prefab(entity.prefab).priority * known.relevance;
			known.priority = owner ? OWNER_PRIORITY : known.priority + rate;
			m_owed.push_back({.priority = known.priority, .rate = rate, .index = index});
		}

		// Removals first: they are small, and they free the client's memory.
		u32 removals = 0;
		while (removals < m_removals.size() && removals < MAX_SECTION_ITEMS && budget >= m_schema.index_bits())
		{
			budget -= m_schema.index_bits();
			room -= m_schema.index_bits();
			++removals;
		}
		m_removals.resize(removals);

		// Then records by priority, while they fit. The most urgent may use the whole packet, so an
		// entity larger than the budget still goes.
		std::sort(m_owed.begin(), m_owed.end(),
				  [](const Owed& a, const Owed& b)
				  {
					  if (a.priority != b.priority)
						  return a.priority > b.priority;
					  return a.rate != b.rate ? a.rate > b.rate : a.index < b.index;
				  });

		// The smallest record there is: an index, the create bit, an age of 0, no previous change, no
		// set and no components.
		const u32 smallest = m_schema.index_bits() + 4;

		u32 records = 0;
		for (u32 i = 0; i < m_owed.size() && records < MAX_SECTION_ITEMS; ++i)
		{
			if (records > 0 && budget < smallest)
				break;

			const u32 bits = record_bits(seat, m_owed[i].index, viewer.known[m_owed[i].index], tick);
			if (bits <= budget || (records == 0 && bits <= room))
			{
				budget = bits <= budget ? budget - bits : 0;
				room -= bits;
				m_owed[records++] = m_owed[i];
			}
		}
		m_owed.resize(records);

		// The section.
		stream.SerializeBits(tick, 32);
		stream.SerializeBits(removals, static_cast<int>(SECTION_COUNT_BITS));
		for (const u32 index : m_removals)
		{
			stream.SerializeBits(index, static_cast<int>(m_schema.index_bits()));

			Known& known	= viewer.known[index];
			known.in_flight = true;
			known.sent		= sequence;
			known.sent_tick = tick;
			viewer.entries.push_back({.index = index, .generation = m_entities[index].generation, .removal = true});
		}

		stream.SerializeBits(records, static_cast<int>(SECTION_COUNT_BITS));
		for (const Owed& owed : m_owed)
		{
			Known& known = viewer.known[owed.index];
			write_record(stream, seat, owed.index, known, tick);

			if (known.presence == Presence::Absent)
			{
				known.presence = Presence::Present;
				++m_entities[owed.index].viewers;
			}

			known.in_flight = true;
			known.sent		= sequence;
			known.sent_tick = tick;
			known.priority	= 0.0f;
			viewer.entries.push_back({.index = owed.index, .generation = m_entities[owed.index].generation});
		}

		viewer.packets.push_back({.sequence = sequence, .tick = tick, .count = removals + records});
	}

	void Replicator::on_notice(u8 seat, const PacketNotice& notice) noexcept
	{
		EMBER_ASSERT(seat < m_viewers.size());
		Viewer& viewer = m_viewers[seat];

		// Notices come oldest first, one for every packet. A packet the replicator wrote no section
		// into has no entry, and an entry older than the notice lost its own.
		while (viewer.packets_head < viewer.packets.size())
		{
			const SentPacket packet = viewer.packets[viewer.packets_head];
			if (sequence_newer(packet.sequence, notice.sequence))
				break;

			++viewer.packets_head;
			const bool match = packet.sequence == notice.sequence;
			settle(viewer, packet, match && notice.delivered);

			if (match)
				break;
		}

		compact(viewer.packets, viewer.packets_head);
		compact(viewer.entries, viewer.entries_head);
	}

	Replicator::Stored* Replicator::find(u32 index, ComponentId component) noexcept
	{
		Pool& pool	 = m_pools[component];
		const u32 at = pool.sparse[index];
		return at == NONE ? nullptr : &pool.values[at];
	}

	const Replicator::Stored* Replicator::find(u32 index, ComponentId component) const noexcept
	{
		const Pool& pool = m_pools[component];
		const u32 at	 = pool.sparse[index];
		return at == NONE ? nullptr : &pool.values[at];
	}

	Replicator::Stored& Replicator::insert(u32 index, ComponentId component) noexcept
	{
		Pool& pool = m_pools[component];
		EMBER_ASSERT(pool.sparse[index] == NONE);

		pool.sparse[index] = static_cast<u32>(pool.dense.size());
		pool.dense.push_back(index);
		return pool.values.emplace_back();
	}

	void Replicator::erase(u32 index, ComponentId component) noexcept
	{
		// The last value takes the place of the one that goes, so the pool stays packed.
		Pool& pool	 = m_pools[component];
		const u32 at = pool.sparse[index];
		EMBER_ASSERT(at != NONE);

		const u32 last = static_cast<u32>(pool.dense.size() - 1);
		if (at != last)
		{
			pool.dense[at]				= pool.dense[last];
			pool.values[at]				= pool.values[last];
			pool.sparse[pool.dense[at]] = at;
		}

		pool.dense.pop_back();
		pool.values.pop_back();
		pool.sparse[index] = NONE;
	}

	bool Replicator::encode(ComponentId component, const void* value, ComponentBits& out) noexcept
	{
		serialize::WriteStream stream = packet_writer(m_scratch);

		[[maybe_unused]] const bool wrote = m_schema.component(component).write(stream, value);
		EMBER_ASSERT(wrote);
		stream.Flush();

		const u32 bits = static_cast<u32>(stream.GetBitsProcessed());
		EMBER_ASSERT(bits <= MAX_COMPONENT_BITS && "a component's wire form is MAX_COMPONENT_BITS at most");
		if (bits > MAX_COMPONENT_BITS)
			return false;

		out.bits = bits;
		std::memcpy(out.bytes.data(), m_scratch.bytes.data(), (bits + 7) / 8);
		return true;
	}

	ComponentMask Replicator::seen(const Entity& entity, bool owner) const noexcept
	{
		return owner ? entity.components : entity.components & ~m_schema.owner_only();
	}

	ComponentMask Replicator::seen_in_prefab(const Entity& entity, bool owner) const noexcept
	{
		const PrefabInfo& info = m_schema.prefab(entity.prefab);
		return owner ? info.components : info.shared;
	}

	bool Replicator::tells_set(const Entity& entity, const Known& known, bool owner) const noexcept
	{
		// A viewer that may not have the entity starts from its prefab; one that does has the set as of acked.
		if (known.acked == NO_TICK)
			return seen(entity, owner) != seen_in_prefab(entity, owner);

		return set_change_for(entity, owner) > known.acked;
	}

	u32 Replicator::record_bits(u8 seat, u32 index, const Known& known, Tick tick) const noexcept
	{
		const Entity& entity = m_entities[index];
		const bool owner	 = entity.owner == seat;

		const Change& change = change_for(entity, owner);

		u32 bits = m_schema.index_bits() + 1;
		if (known.acked == NO_TICK)
			bits += m_schema.prefab_bits() + NetId::GENERATION_BITS + 2 + (owner && entity.key != 0 ? 32 : 0);

		bits += detail::varint_bits(tick - change.latest) + 1;
		if (change.previous != NO_TICK)
			bits += detail::varint_bits(change.latest - change.previous - 1);

		const ComponentMask components = seen(entity, owner);

		bits += 1;
		if (tells_set(entity, known, owner))
		{
			const u32 differ = detail::component_count(components ^ seen_in_prefab(entity, owner));
			bits += detail::varint_bits(differ) + differ * m_schema.component_bits();
		}

		bits += detail::component_count(components);
		for (ComponentMask left = components; left != 0;)
		{
			const Stored& stored = *find(index, detail::take_lowest(left));
			if (stored.changed > known.acked)
				bits += stored.wire.bits;
		}

		return bits;
	}

	void Replicator::write_record(serialize::WriteStream& stream, u8 seat, u32 index, const Known& known,
								  Tick tick) noexcept
	{
		const Entity& entity = m_entities[index];
		const bool owner	 = entity.owner == seat;

		[[maybe_unused]] const i64 start = stream.GetBitsProcessed();

		stream.SerializeBits(index, static_cast<int>(m_schema.index_bits()));

		// Until a record is known to have arrived, the viewer may not have the entity: every record
		// says what it is.
		const bool create = known.acked == NO_TICK;
		stream.SerializeBits(create ? 1u : 0u, 1);
		if (create)
		{
			if (m_schema.prefab_bits() > 0)
				stream.SerializeBits(entity.prefab, static_cast<int>(m_schema.prefab_bits()));

			const bool key = owner && entity.key != 0;
			stream.SerializeBits(entity.generation, static_cast<int>(NetId::GENERATION_BITS));
			stream.SerializeBits(owner ? 1u : 0u, 1);
			stream.SerializeBits(key ? 1u : 0u, 1);
			if (key)
				stream.SerializeBits(entity.key, 32);
		}

		// When the entity's state began, as this viewer sees it, and when the one before it did: the
		// client's samples.
		const Change& change = change_for(entity, owner);
		u32 age				 = tick - change.latest;
		(void)detail::serialize_varint(stream, age);

		stream.SerializeBits(change.previous != NO_TICK ? 1u : 0u, 1);
		if (change.previous != NO_TICK)
		{
			u32 gap = change.latest - change.previous - 1;
			(void)detail::serialize_varint(stream, gap);
		}

		// Its components, when the viewer's may differ: told whole, as those that differ from the
		// prefab's, so no earlier record needs to have arrived.
		const ComponentMask components = seen(entity, owner);
		const bool set				   = tells_set(entity, known, owner);
		stream.SerializeBits(set ? 1u : 0u, 1);
		if (set)
		{
			const ComponentMask differ = components ^ seen_in_prefab(entity, owner);
			u32 count				   = detail::component_count(differ);
			(void)detail::serialize_varint(stream, count);

			for (ComponentMask left = differ; left != 0;)
			{
				const u32 id = detail::take_lowest(left);
				if (m_schema.component_bits() > 0)
					stream.SerializeBits(id, static_cast<int>(m_schema.component_bits()));
			}
		}

		// Which of them follow: those changed since the viewer's state.
		ComponentMask mask = 0;
		u32 place		   = 0;
		for (ComponentMask left = components; left != 0; ++place)
		{
			if (find(index, detail::take_lowest(left))->changed > known.acked)
				mask |= ComponentMask{1} << place;
		}
		(void)detail::serialize_mask(stream, mask, detail::component_count(components));

		for (ComponentMask left = components; left != 0;)
		{
			const Stored& stored = *find(index, detail::take_lowest(left));
			if (stored.changed > known.acked)
				copy_bits(stream, stored.wire);
		}

		EMBER_ASSERT(stream.GetBitsProcessed() - start == record_bits(seat, index, known, tick) &&
					 "record_bits() and write_record() disagree");
	}

	void Replicator::settle(Viewer& viewer, const SentPacket& packet, bool delivered) noexcept
	{
		for (u32 i = 0; i < packet.count; ++i)
		{
			const Sent sent = viewer.entries[viewer.entries_head++];
			Known& known	= viewer.known[sent.index];

			// The index's entity was let go and the index reused since: the entry is about a past entity.
			if (m_entities[sent.index].generation != sent.generation)
				continue;

			const bool newest = known.in_flight && known.sent == packet.sequence;

			if (sent.removal)
			{
				if (known.presence != Presence::Removing)
					continue;

				if (delivered)
				{
					known = {.relevance = known.relevance};
					release(sent.index);
				}
				else if (newest)
				{
					known.in_flight = false;
				}

				continue;
			}

			if (known.presence != Presence::Present)
				continue;

			if (delivered)
				known.acked = std::max(known.acked, packet.tick);

			if (newest)
				known.in_flight = false;
		}
	}

	void Replicator::release(u32 index) noexcept
	{
		Entity& entity = m_entities[index];
		EMBER_ASSERT(entity.viewers > 0);

		--entity.viewers;
		if (!entity.alive && entity.used && entity.viewers == 0)
			free_index(index);
	}

	void Replicator::free_index(u32 index) noexcept
	{
		Entity& entity	  = m_entities[index];
		entity.used		  = false;
		entity.generation = entity.generation == NetId::MAX_GENERATION ? 1 : entity.generation + 1;

		m_free[(m_free_head + m_free_count) % m_free.size()] = index;
		++m_free_count;
	}
}
