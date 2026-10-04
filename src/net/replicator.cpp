#include <ember/core/logger.h>
#include <ember/memory/memory.h>
#include <ember/net/replicator.h>

#include <algorithm>
#include <limits>
#include <utility>

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

	Replicator::Replicator(ecs::World& world, const ReplicatorDef& def) noexcept
		: m_world(world), m_def(def), m_schema(world, def.max_entities), m_entities(&memory::heap(MemoryTag::Network)),
		  m_pools(&memory::heap(MemoryTag::Network)), m_viewers(&memory::heap(MemoryTag::Network)),
		  m_free(&memory::heap(MemoryTag::Network)), m_spawned(&memory::heap(MemoryTag::Network)),
		  m_owed(&memory::heap(MemoryTag::Network)), m_removals(&memory::heap(MemoryTag::Network))
	{
		EMBER_ASSERT(def.max_viewers > 0 && def.max_viewers < NO_OWNER);
		EMBER_ASSERT(world.role() == ecs::Role::Server && "a replicator follows a server world");

		entt::registry& registry = world.registry;
		registry.on_construct<ecs::PrefabRef>().connect<&Replicator::spawned>(*this);
		(void)registry.storage<NetId>();
		(void)registry.storage<Owner>();
		(void)registry.storage<Priority>();

		const u32 count = m_schema.max_entities();
		m_entities.resize(count);

		// A pool per component type, empty until entities have them.
		m_pools.reserve(m_schema.component_count());
		for (u32 id = 0; id < m_schema.component_count(); ++id)
		{
			const ecs::ComponentInfo& info = m_schema.component(static_cast<ComponentId>(id));
			info.assure(registry);

			Pool& pool = m_pools.emplace_back(Pool{.info	= &info,
												   .storage = std::as_const(registry).storage(info.type),
												   .sparse	= Vector<u32>(&memory::heap(MemoryTag::Network)),
												   .dense	= Vector<u32>(&memory::heap(MemoryTag::Network)),
												   .values	= Vector<Stored>(&memory::heap(MemoryTag::Network)),
												   .bytes	= Vector<u8>(&memory::heap(MemoryTag::Network))});
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

		// What the world spawned before the replicator was made replicates from the first update too.
		for (const ecs::Entity entity : registry.view<ecs::PrefabRef>())
			m_spawned.push_back(entity);
	}

	Replicator::~Replicator() noexcept { m_world.registry.on_construct<ecs::PrefabRef>().disconnect(this); }

	void Replicator::spawned(entt::registry&, ecs::Entity entity) noexcept { m_spawned.push_back(entity); }

	void Replicator::update(Tick tick) noexcept
	{
		EMBER_ASSERT(tick != NO_TICK);
		if (tick == m_updated)
			return;

		m_updated					   = tick;
		const entt::registry& registry = m_world.registry;

		// Every replicated entity, whole: gone, or its components and their values as they are now. The
		// destroyed go first, so a spawn of the same tick may have an index one of them let go.
		for (u32 index = 0; index < m_high; ++index)
		{
			const Slot& slot = m_entities[index];
			if (!slot.alive)
				continue;

			if (registry.valid(slot.entity))
				match(index, tick);
			else
				destroy(index);
		}

		// Entities spawned since, from prefabs that replicate. One gone already never travels.
		for (const ecs::Entity entity : m_spawned)
		{
			if (!registry.valid(entity) || registry.all_of<NetId>(entity))
				continue;

			const ecs::PrefabRef* prefab = registry.try_get<ecs::PrefabRef>(entity);
			if (prefab != nullptr && m_schema.replicated(static_cast<PrefabId>(prefab->id)))
				create(entity, tick);
		}
		m_spawned.clear();
	}

	void Replicator::create(ecs::Entity entity, Tick tick) noexcept
	{
		if (m_free_count == 0)
		{
			EMBER_WARN("more replicated entities than ReplicatorDef::max_entities: one stays on the server");
			return;
		}

		const u32 index = m_free[m_free_head];
		m_free_head		= (m_free_head + 1) % static_cast<u32>(m_free.size());
		--m_free_count;

		Slot& slot = m_entities[index];
		EMBER_ASSERT(!slot.used);

		entt::registry& registry  = m_world.registry;
		const PrefabId prefab	  = static_cast<PrefabId>(registry.get<ecs::PrefabRef>(entity).id);
		const PrefabInfo& info	  = m_schema.prefab(prefab);
		const Owner* owner		  = registry.try_get<Owner>(entity);
		const ecs::Prefab& values = m_world.prefabs()[prefab];

		EMBER_ASSERT((owner == nullptr || owner->seat == NO_OWNER || owner->seat < m_viewers.size()) &&
					 "an Owner seat the server has");

		slot.entity		= entity;
		slot.all		= {.latest = tick};
		slot.shared		= {.latest = tick};
		slot.set_all	= NO_TICK;
		slot.set_shared = NO_TICK;
		slot.components = info.components;
		slot.key		= owner != nullptr ? owner->key : 0;
		slot.prefab		= prefab;
		slot.owner		= owner != nullptr && owner->seat < m_viewers.size() ? owner->seat : NO_OWNER;
		slot.viewers	= 0;
		slot.alive		= true;
		slot.used		= true;

		// The prefab's values, which every client has already: unchanged, they never travel.
		for (ComponentMask left = info.components; left != 0;)
		{
			const ComponentId component = detail::take_lowest(left);
			const ecs::ComponentId type = m_schema.component(component).id;
			Stored& stored				= insert(index, component, values.find(type)->value.data());
			stored.changed				= NO_TICK;
			stored.wire					= m_schema.prefab_wire(prefab, component);
		}

		// Every viewer let go of the index's last entity before it was freed.
		for (Viewer& viewer : m_viewers)
			viewer.known[index] = {};

		registry.emplace_or_replace<NetId>(entity, NetId::make(index, slot.generation));

		m_high = std::max(m_high, index + 1);
		++m_alive;

		// What the spawn set over the prefab: the slot's first changes.
		match(index, tick, true);
	}

	void Replicator::match(u32 index, Tick tick, bool every) noexcept
	{
		Slot& slot				 = m_entities[index];
		entt::registry& registry = m_world.registry;

		// Its component set: one gained comes with its value, one lost goes.
		ComponentMask has = 0;
		for (u32 component = 0; component < m_pools.size(); ++component)
		{
			if (m_pools[component].storage->contains(slot.entity))
				has |= component_bit(static_cast<ComponentId>(component));
		}

		const ComponentMask changed_set = has ^ slot.components;
		if (changed_set != 0)
		{
			EMBER_ASSERT(tick >= slot.all.latest && "update() goes forward in time");

			for (ComponentMask lost = slot.components & ~has; lost != 0;)
				erase(index, detail::take_lowest(lost));

			for (ComponentMask gained = has & ~slot.components; gained != 0;)
			{
				const ComponentId component = detail::take_lowest(gained);
				const void* value			= m_pools[component].info->find(registry, slot.entity);
				Stored& stored				= insert(index, component, value);
				stored.changed				= tick;
				if (!encode(component, value, stored.wire))
				{
					erase(index, component); // too large for the wire: the assert said so
					has &= ~component_bit(component);
				}
				else if ((m_schema.predicted() & component_bit(component)) != 0)
				{
					snap(index, component, stored.wire);
				}
			}

			// A different set of components is a change of state, for whoever gets the ones that changed.
			slot.components = has;
			slot.set_all	= tick;
			slot.all.at(tick);
			if ((changed_set & ~m_schema.owner_only()) != 0)
			{
				slot.set_shared = tick;
				slot.shared.at(tick);
			}
		}

		// Its values: bytes nobody touched are skipped, and only a change on the wire counts as a change.
		for (ComponentMask left = slot.components; left != 0;)
		{
			const ComponentId component = detail::take_lowest(left);
			Pool& pool					= m_pools[component];
			const u32 size				= pool.info->size;
			const u32 at				= pool.sparse[index];
			const void* value			= pool.info->find(registry, slot.entity);
			u8* bytes					= pool.bytes.data() + static_cast<size_t>(at) * size;

			if (!every && std::memcmp(bytes, value, size) == 0)
				continue;

			std::memcpy(bytes, value, size);

			ComponentBits wire;
			if (!encode(component, value, wire))
				continue;

			if ((m_schema.predicted() & component_bit(component)) != 0)
				snap(index, component, wire);

			// The writer leaves the bits past the last one clear, so equal values have equal bytes.
			Stored& stored = pool.values[at];
			if (wire.bits == stored.wire.bits &&
				std::memcmp(wire.bytes.data(), stored.wire.bytes.data(), (wire.bits + 7) / 8) == 0)
				continue;

			EMBER_ASSERT(tick >= slot.all.latest && "update() goes forward in time");
			stored.wire	   = wire;
			stored.changed = tick;

			slot.all.at(tick);
			if ((m_schema.owner_only() & component_bit(component)) == 0)
				slot.shared.at(tick);
		}
	}

	void Replicator::snap(u32 index, ComponentId component, const ComponentBits& wire) noexcept
	{
		Pool& pool	= m_pools[component];
		void* value = pool.info->get(m_world.registry, m_entities[index].entity);

		serialize::ReadStream reader(wire.bytes.data(), static_cast<int>((wire.bits + 7) / 8));
		[[maybe_unused]] const bool read = pool.info->read(reader, value);
		EMBER_ASSERT(read && "a component's own bits do not read back");

		std::memcpy(pool.bytes.data() + static_cast<size_t>(pool.sparse[index]) * pool.info->size, value,
					pool.info->size);
	}

	void Replicator::destroy(u32 index) noexcept
	{
		Slot& slot	= m_entities[index];
		slot.alive	= false;
		slot.entity = ecs::NO_ENTITY;
		--m_alive;

		// A removal carries no values: they go now.
		for (ComponentMask left = slot.components; left != 0;)
			erase(index, detail::take_lowest(left));
		slot.components = 0;

		for (Viewer& viewer : m_viewers)
		{
			Known& known = viewer.known[index];
			if (known.presence == Presence::Present)
			{
				known.presence	= Presence::Removing;
				known.in_flight = false;
			}
		}

		if (slot.viewers == 0)
			free_index(index);
	}

	ecs::Entity Replicator::entity(NetId id) const noexcept
	{
		const u32 index = id.index();
		if (!id || index >= m_entities.size())
			return ecs::NO_ENTITY;

		const Slot& slot = m_entities[index];
		return slot.alive && slot.generation == id.generation() ? slot.entity : ecs::NO_ENTITY;
	}

	NetId Replicator::id(ecs::Entity entity) const noexcept
	{
		const NetId* id = m_world.registry.valid(entity) ? m_world.registry.try_get<NetId>(entity) : nullptr;
		return id != nullptr && this->entity(*id) == entity ? *id : NO_NET_ID;
	}

	void Replicator::set_relevance(u8 viewer, NetId id, f32 relevance) noexcept
	{
		EMBER_ASSERT(viewer < m_viewers.size());
		EMBER_ASSERT(relevance >= 0.0f);

		if (entity(id) != ecs::NO_ENTITY && viewer < m_viewers.size())
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
		const auto* priorities = std::as_const(m_world.registry).storage<Priority>(); // made with the replicator
		m_owed.clear();
		m_removals.clear();

		for (u32 index = 0; index < m_high; ++index)
		{
			Known& known	 = viewer.known[index];
			const Slot& slot = m_entities[index];

			if (known.presence == Presence::Removing)
			{
				if (!known.in_flight)
					m_removals.push_back(index);
				continue;
			}

			if (!slot.alive)
				continue;

			const bool owner = slot.owner == seat;
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
							  slot.shared.latest > (known.in_flight ? known.sent_tick : known.acked);
			if (!owed)
				continue;

			const Priority* priority = priorities->contains(slot.entity) ? &priorities->get(slot.entity) : nullptr;
			const f32 rate			 = (priority != nullptr ? priority->value : 1.0f) * known.relevance;
			known.priority			 = owner ? OWNER_PRIORITY : known.priority + rate;
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

	Replicator::Stored& Replicator::insert(u32 index, ComponentId component, const void* bytes) noexcept
	{
		Pool& pool	   = m_pools[component];
		const u32 size = pool.info->size;
		EMBER_ASSERT(pool.sparse[index] == NONE);

		pool.sparse[index] = static_cast<u32>(pool.dense.size());
		pool.dense.push_back(index);
		pool.bytes.resize(pool.bytes.size() + size);
		std::memcpy(pool.bytes.data() + pool.bytes.size() - size, bytes, size);
		return pool.values.emplace_back();
	}

	void Replicator::erase(u32 index, ComponentId component) noexcept
	{
		// The last value takes the place of the one that goes, so the pool stays packed.
		Pool& pool	 = m_pools[component];
		const u32 at = pool.sparse[index];
		EMBER_ASSERT(at != NONE);

		const u32 size = pool.info->size;
		const u32 last = static_cast<u32>(pool.dense.size() - 1);
		if (at != last)
		{
			pool.dense[at]				= pool.dense[last];
			pool.values[at]				= pool.values[last];
			pool.sparse[pool.dense[at]] = at;
			std::memcpy(pool.bytes.data() + static_cast<size_t>(at) * size,
						pool.bytes.data() + static_cast<size_t>(last) * size, size);
		}

		pool.dense.pop_back();
		pool.values.pop_back();
		pool.bytes.resize(pool.bytes.size() - size);
		pool.sparse[index] = NONE;
	}

	bool Replicator::encode(ComponentId component, const void* value, ComponentBits& out) noexcept
	{
		serialize::WriteStream stream = packet_writer(m_scratch);

		[[maybe_unused]] const bool wrote = m_pools[component].info->write(stream, value);
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

	ComponentMask Replicator::seen(const Slot& slot, bool owner) const noexcept
	{
		return owner ? slot.components : slot.components & ~m_schema.owner_only();
	}

	ComponentMask Replicator::seen_in_prefab(const Slot& slot, bool owner) const noexcept
	{
		const PrefabInfo& info = m_schema.prefab(slot.prefab);
		return owner ? info.components : info.shared;
	}

	bool Replicator::tells_set(const Slot& slot, const Known& known, bool owner) const noexcept
	{
		// A viewer that may not have the entity starts from its prefab; one that does has the set as of acked.
		if (known.acked == NO_TICK)
			return seen(slot, owner) != seen_in_prefab(slot, owner);

		return set_change_for(slot, owner) > known.acked;
	}

	u32 Replicator::record_bits(u8 seat, u32 index, const Known& known, Tick tick) const noexcept
	{
		const Slot& slot = m_entities[index];
		const bool owner = slot.owner == seat;

		const Change& change = change_for(slot, owner);

		u32 bits = m_schema.index_bits() + 1;
		if (known.acked == NO_TICK)
			bits += m_schema.prefab_bits() + NetId::GENERATION_BITS + 2 + (owner && slot.key != 0 ? 32 : 0);

		bits += detail::varint_bits(tick - change.latest) + 1;
		if (change.previous != NO_TICK)
			bits += detail::varint_bits(change.latest - change.previous - 1);

		const ComponentMask components = seen(slot, owner);

		bits += 1;
		if (tells_set(slot, known, owner))
		{
			const u32 differ = detail::component_count(components ^ seen_in_prefab(slot, owner));
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
		const Slot& slot = m_entities[index];
		const bool owner = slot.owner == seat;

		[[maybe_unused]] const i64 start = stream.GetBitsProcessed();

		stream.SerializeBits(index, static_cast<int>(m_schema.index_bits()));

		// Until a record is known to have arrived, the viewer may not have the slot: every record
		// says what it is.
		const bool create = known.acked == NO_TICK;
		stream.SerializeBits(create ? 1u : 0u, 1);
		if (create)
		{
			if (m_schema.prefab_bits() > 0)
				stream.SerializeBits(slot.prefab, static_cast<int>(m_schema.prefab_bits()));

			const bool key = owner && slot.key != 0;
			stream.SerializeBits(slot.generation, static_cast<int>(NetId::GENERATION_BITS));
			stream.SerializeBits(owner ? 1u : 0u, 1);
			stream.SerializeBits(key ? 1u : 0u, 1);
			if (key)
				stream.SerializeBits(slot.key, 32);
		}

		// When the slot's state began, as this viewer sees it, and when the one before it did: the
		// client's samples.
		const Change& change = change_for(slot, owner);
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
		const ComponentMask components = seen(slot, owner);
		const bool set				   = tells_set(slot, known, owner);
		stream.SerializeBits(set ? 1u : 0u, 1);
		if (set)
		{
			const ComponentMask differ = components ^ seen_in_prefab(slot, owner);
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

			// The index's slot was let go and the index reused since: the entry is about a past slot.
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
		Slot& slot = m_entities[index];
		EMBER_ASSERT(slot.viewers > 0);

		--slot.viewers;
		if (!slot.alive && slot.used && slot.viewers == 0)
			free_index(index);
	}

	void Replicator::free_index(u32 index) noexcept
	{
		Slot& slot		= m_entities[index];
		slot.used		= false;
		slot.generation = slot.generation == NetId::MAX_GENERATION ? 1 : slot.generation + 1;

		m_free[(m_free_head + m_free_count) % m_free.size()] = index;
		++m_free_count;
	}
}
