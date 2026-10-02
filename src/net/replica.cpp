#include <ember/memory/memory.h>
#include <ember/net/replica.h>

#include <algorithm>

namespace ember::net
{
	Replica::Replica(const Schema& schema) noexcept
		: m_schema(schema), m_pools(&memory::heap(MemoryTag::Network)), m_entities(&memory::heap(MemoryTag::Network)),
		  m_events(&memory::heap(MemoryTag::Network)), m_staged_removals(&memory::heap(MemoryTag::Network)),
		  m_staged(&memory::heap(MemoryTag::Network)), m_staged_values(&memory::heap(MemoryTag::Network))
	{
		// A pool per component type, empty until entities have them.
		m_pools.reserve(schema.component_count());
		for (u32 id = 0; id < schema.component_count(); ++id)
		{
			if (has_any(schema.component(static_cast<ComponentId>(id)).flags, ComponentFlags::Interpolated))
				m_interpolated |= component_bit(static_cast<ComponentId>(id));

			Pool& pool = m_pools.emplace_back(Pool{.sparse	= Vector<u32>(&memory::heap(MemoryTag::Network)),
												   .dense	= Vector<u32>(&memory::heap(MemoryTag::Network)),
												   .values	= Vector<u8>(&memory::heap(MemoryTag::Network)),
												   .samples = Vector<u8>(&memory::heap(MemoryTag::Network))});
			pool.sparse.resize(schema.max_entities(), NONE);
		}

		m_entities.resize(schema.max_entities());
	}

	void Replica::reset() noexcept
	{
		for (Entity& entity : m_entities)
			entity = {};

		for (Pool& pool : m_pools)
		{
			std::fill(pool.sparse.begin(), pool.sparse.end(), NONE);
			pool.dense.clear();
			pool.values.clear();
			pool.samples.clear();
		}

		m_alive	 = 0;
		m_latest = NO_TICK;
		m_events.clear();
		m_events_head = 0;
	}

	bool Replica::read(serialize::ReadStream& stream) noexcept
	{
		m_staged_removals.clear();
		m_staged.clear();
		m_staged_values.clear();

		Tick tick = NO_TICK;
		if (!parse(stream, tick))
			return false;

		apply(tick);
		return true;
	}

	bool Replica::poll(ReplicaEvent& event) noexcept
	{
		if (m_events_head == m_events.size())
		{
			m_events.clear();
			m_events_head = 0;
			return false;
		}

		event = m_events[m_events_head++];
		return true;
	}

	bool Replica::alive(NetId id) const noexcept
	{
		const u32 index = id.index();
		return id && index < m_entities.size() && m_entities[index].alive &&
			   m_entities[index].generation == id.generation();
	}

	PrefabId Replica::prefab(NetId id) const noexcept
	{
		EMBER_ASSERT(alive(id));
		return m_entities[id.index()].prefab;
	}

	bool Replica::owned(NetId id) const noexcept { return alive(id) && m_entities[id.index()].owned; }

	ComponentMask Replica::components(NetId id) const noexcept
	{
		return alive(id) ? m_entities[id.index()].components : 0;
	}

	Tick Replica::tick(NetId id) const noexcept { return alive(id) ? m_entities[id.index()].tick : NO_TICK; }

	bool Replica::get(NetId id, ComponentId component, void* out) const noexcept
	{
		if (!alive(id) || component >= m_pools.size())
			return false;

		const u8* value = value_of(id.index(), component);
		if (value == nullptr)
			return false;

		std::memcpy(out, value, m_schema.component(component).size);
		return true;
	}

	bool Replica::sample(NetId id, ComponentId component, f64 tick, void* from, void* to, f32& t) const noexcept
	{
		if (!alive(id) || component >= m_pools.size())
			return false;

		const u32 index	   = id.index();
		const u8* newest   = value_of(index, component);
		const u32 size	   = m_schema.component(component).size;
		const bool sampled = (m_interpolated & component_bit(component)) != 0;
		if (newest == nullptr)
			return false;

		EMBER_ASSERT(sampled && "sample() is for Interpolated components; get() reads the rest");
		t = 0.0f;

		const Entity& entity = m_entities[index];
		if (!sampled || entity.samples == 0)
		{
			std::memcpy(from, newest, size);
			std::memcpy(to, newest, size);
			return true;
		}

		// The newest sample at or before tick: the one to draw from.
		u32 at = 0;
		while (at + 1 < entity.samples && static_cast<f64>(entity.ticks[place_of(entity, at + 1)]) <= tick)
			++at;

		std::memcpy(from, sample_of(index, component, place_of(entity, at)), size);

		const Tick start   = entity.ticks[place_of(entity, at)];
		const bool between = at + 1 < entity.samples && static_cast<f64>(start) <= tick;
		if (!between)
		{
			std::memcpy(to, from, size); // before the oldest or past the newest: hold it
			return true;
		}

		const Tick end = entity.ticks[place_of(entity, at + 1)];
		std::memcpy(to, sample_of(index, component, place_of(entity, at + 1)), size);
		t = static_cast<f32>((tick - static_cast<f64>(start)) / static_cast<f64>(end - start));
		return true;
	}

	ComponentMask Replica::seen_in_prefab(PrefabId prefab, bool owned) const noexcept
	{
		const PrefabInfo& info = m_schema.prefab(prefab);
		return owned ? info.components : info.shared;
	}

	bool Replica::parse(serialize::ReadStream& stream, Tick& tick) noexcept
	{
		using Stream = serialize::ReadStream; // what the serialize macros expect

		const u32 index_bits	 = m_schema.index_bits();
		const u32 prefab_bits	 = m_schema.prefab_bits();
		const u32 component_bits = m_schema.component_bits();

		// Sections come in tick order: a server never goes back in time.
		serialize_bits(stream, tick, 32);
		if (tick == NO_TICK || (m_latest != NO_TICK && tick < m_latest))
			return false;

		u32 removals = 0;
		serialize_bits(stream, removals, SECTION_COUNT_BITS);
		for (u32 i = 0; i < removals; ++i)
		{
			u32 index = 0;
			serialize_bits(stream, index, index_bits);
			if (index >= m_schema.max_entities())
				return false;

			m_staged_removals.push_back(index);
		}

		u32 records = 0;
		serialize_bits(stream, records, SECTION_COUNT_BITS);
		for (u32 i = 0; i < records; ++i)
		{
			Staged staged;
			serialize_bits(stream, staged.index, index_bits);
			if (staged.index >= m_schema.max_entities())
				return false;

			// What the entity has before this record: its prefab's components for a new one.
			ComponentMask had = 0;

			serialize_bool(stream, staged.create);
			if (staged.create)
			{
				u32 prefab = 0;
				if (prefab_bits > 0)
					serialize_bits(stream, prefab, prefab_bits);

				u32 generation = 0;
				serialize_bits(stream, generation, NetId::GENERATION_BITS);
				if (prefab >= m_schema.prefab_count() || generation == 0)
					return false;

				bool key = false;
				serialize_bool(stream, staged.owned);
				serialize_bool(stream, key);
				if (key)
					serialize_bits(stream, staged.key, 32);

				staged.prefab	  = static_cast<PrefabId>(prefab);
				staged.generation = static_cast<u16>(generation);
				had				  = seen_in_prefab(staged.prefab, staged.owned);
			}
			else
			{
				// Only a create brings an entity this client does not have.
				const Entity& entity = m_entities[staged.index];
				if (!entity.alive)
					return false;

				staged.prefab	  = entity.prefab;
				staged.generation = entity.generation;
				staged.owned	  = entity.owned;
				had				  = entity.components;
			}

			u32 age = 0;
			if (!detail::serialize_varint(stream, age) || age >= tick)
				return false;

			staged.changed = tick - age;

			bool previous = false;
			serialize_bool(stream, previous);
			if (previous)
			{
				u32 gap = 0;
				if (!detail::serialize_varint(stream, gap) || gap >= staged.changed - 1)
					return false;

				staged.previous = staged.changed - gap - 1;
			}

			// Its components: as they were, or told whole as those that differ from its prefab's.
			staged.components = had;

			bool set = false;
			serialize_bool(stream, set);
			if (set)
			{
				u32 count = 0;
				if (!detail::serialize_varint(stream, count) || count > m_schema.component_count())
					return false;

				ComponentMask differ = 0;
				for (u32 n = 0; n < count; ++n)
				{
					u32 id = 0;
					if (component_bits > 0)
						serialize_bits(stream, id, component_bits);

					// Ascending, each once.
					if (id >= m_schema.component_count() || (differ >> id) != 0)
						return false;

					differ |= component_bit(static_cast<ComponentId>(id));
				}

				staged.components = seen_in_prefab(staged.prefab, staged.owned) ^ differ;
			}

			// Another client's private components never reach this one.
			if (!staged.owned && (staged.components & m_schema.owner_only()) != 0)
				return false;

			ComponentMask mask = 0;
			if (!detail::serialize_mask(stream, mask, detail::component_count(staged.components)))
				return false;

			staged.values = static_cast<u32>(m_staged_values.size());
			u32 place	  = 0;
			for (ComponentMask left = staged.components; left != 0; ++place)
			{
				const ComponentId component = detail::take_lowest(left);
				if ((mask & (ComponentMask{1} << place)) == 0)
					continue;

				staged.carried |= component_bit(component);

				const ComponentCodec& codec = m_schema.component(component);
				const size_t at				= m_staged_values.size();
				m_staged_values.resize(at + codec.size);
				if (!codec.read(stream, m_staged_values.data() + at))
					return false;
			}

			// A component the entity gains comes with its value: the server changed it after anything
			// this client has.
			if ((staged.components & ~had & ~staged.carried) != 0)
				return false;

			m_staged.push_back(staged);
		}

		return true;
	}

	void Replica::apply(Tick tick) noexcept
	{
		for (const u32 index : m_staged_removals)
		{
			if (m_entities[index].alive)
				remove(index, tick);
		}

		for (const Staged& staged : m_staged)
		{
			Entity& entity = m_entities[staged.index];

			if (staged.create && (!entity.alive || entity.generation != staged.generation))
			{
				if (entity.alive)
					remove(staged.index, tick); // only a broken server reuses an index this client still has

				spawn(staged, tick);
			}
			else if (!entity.alive)
			{
				continue; // removed by this very section: only a broken server sends that
			}

			// Its components as the record has them: a lost one goes, a gained one arrives with its value.
			const ComponentMask lost   = entity.components & ~staged.components;
			const ComponentMask gained = staged.components & ~entity.components;

			for (ComponentMask left = lost; left != 0;)
				erase(staged.index, detail::take_lowest(left));

			for (ComponentMask left = gained; left != 0;)
			{
				const ComponentId component = detail::take_lowest(left);
				insert(staged.index, component, m_schema.component(component).empty.data());
			}

			entity.components = staged.components;

			u32 at = staged.values;
			for (ComponentMask left = staged.carried; left != 0;)
			{
				const ComponentId component = detail::take_lowest(left);
				const u32 size				= m_schema.component(component).size;
				std::memcpy(value_of(staged.index, component), m_staged_values.data() + at, size);
				at += size;
			}

			// A component gained part way through the entity's life is drawn at its value for as far
			// back as the samples go, not slid in from a default it never had.
			for (ComponentMask left = gained & m_interpolated; left != 0;)
			{
				const ComponentId component = detail::take_lowest(left);
				const u32 size				= m_schema.component(component).size;
				for (u32 place = 0; place < REPLICA_SAMPLES; ++place)
					std::memcpy(sample_of(staged.index, component, place), value_of(staged.index, component), size);
			}

			// A new state: sample it where it began, and when the state before it is known to have held
			// until then, close that one off just before.
			const ComponentMask sampled = entity.components & m_interpolated;
			const bool newer =
				entity.samples == 0 || staged.changed > entity.ticks[place_of(entity, entity.samples - 1u)];
			if (sampled != 0 && staged.changed != entity.since && newer)
			{
				if (entity.samples > 0 && staged.previous == entity.since &&
					staged.changed - 1 > entity.ticks[place_of(entity, entity.samples - 1u)])
				{
					const u32 last	= place_of(entity, entity.samples - 1u);
					const u32 place = push_sample(entity, staged.changed - 1);
					for (ComponentMask left = sampled; left != 0;)
					{
						const ComponentId component = detail::take_lowest(left);
						std::memcpy(sample_of(staged.index, component, place), sample_of(staged.index, component, last),
									m_schema.component(component).size);
					}
				}

				const u32 place = push_sample(entity, staged.changed);
				for (ComponentMask left = sampled; left != 0;)
				{
					const ComponentId component = detail::take_lowest(left);
					std::memcpy(sample_of(staged.index, component, place), value_of(staged.index, component),
								m_schema.component(component).size);
				}
			}

			if ((lost | gained | staged.carried) != 0)
			{
				m_events.push_back({.kind	 = ReplicaEventKind::Updated,
									.id		 = NetId::make(staged.index, entity.generation),
									.prefab	 = entity.prefab,
									.owned	 = entity.owned,
									.tick	 = tick,
									.added	 = gained,
									.removed = lost,
									.written = staged.carried});
			}

			entity.since = staged.changed;
			entity.tick	 = tick;
		}

		m_latest = tick;
	}

	void Replica::spawn(const Staged& staged, Tick tick) noexcept
	{
		Entity& entity	  = m_entities[staged.index];
		entity			  = {};
		entity.key		  = staged.key;
		entity.prefab	  = staged.prefab;
		entity.generation = staged.generation;
		entity.alive	  = true;
		entity.owned	  = staged.owned;

		// Its prefab's components at the prefab's values: a record carries only what differs from them.
		entity.components = seen_in_prefab(staged.prefab, staged.owned);
		for (ComponentMask left = entity.components; left != 0;)
		{
			const ComponentId component = detail::take_lowest(left);
			insert(staged.index, component, m_schema.prefab_value(staged.prefab, component).bytes.data());
		}

		++m_alive;
		m_events.push_back({.kind	= ReplicaEventKind::Created,
							.id		= NetId::make(staged.index, staged.generation),
							.prefab = staged.prefab,
							.owned	= staged.owned,
							.key	= staged.key,
							.tick	= tick});
	}

	void Replica::remove(u32 index, Tick tick) noexcept
	{
		Entity& entity = m_entities[index];
		for (ComponentMask left = entity.components; left != 0;)
			erase(index, detail::take_lowest(left));

		entity.components = 0;
		entity.alive	  = false;
		--m_alive;

		m_events.push_back({.kind	= ReplicaEventKind::Removed,
							.id		= NetId::make(index, entity.generation),
							.prefab = entity.prefab,
							.owned	= entity.owned,
							.tick	= tick});
	}

	void Replica::insert(u32 index, ComponentId component, const void* value) noexcept
	{
		Pool& pool	   = m_pools[component];
		const u32 size = m_schema.component(component).size;
		EMBER_ASSERT(pool.sparse[index] == NONE);

		const u32 at	   = static_cast<u32>(pool.dense.size());
		pool.sparse[index] = at;
		pool.dense.push_back(index);

		pool.values.resize(pool.values.size() + size);
		std::memcpy(pool.values.data() + static_cast<size_t>(at) * size, value, size);

		if ((m_interpolated & component_bit(component)) != 0)
		{
			pool.samples.resize(pool.samples.size() + static_cast<size_t>(REPLICA_SAMPLES) * size);
			for (u32 place = 0; place < REPLICA_SAMPLES; ++place)
				std::memcpy(sample_of(index, component, place), value, size);
		}
	}

	void Replica::erase(u32 index, ComponentId component) noexcept
	{
		// The last value takes the place of the one that goes, so the pool stays packed.
		Pool& pool	   = m_pools[component];
		const u32 size = m_schema.component(component).size;
		const u32 at   = pool.sparse[index];
		EMBER_ASSERT(at != NONE);

		const u32 last = static_cast<u32>(pool.dense.size() - 1);
		if (at != last)
		{
			pool.dense[at]				= pool.dense[last];
			pool.sparse[pool.dense[at]] = at;
			std::memcpy(pool.values.data() + static_cast<size_t>(at) * size,
						pool.values.data() + static_cast<size_t>(last) * size, size);

			if (!pool.samples.empty())
			{
				const size_t ring = static_cast<size_t>(REPLICA_SAMPLES) * size;
				std::memcpy(pool.samples.data() + at * ring, pool.samples.data() + last * ring, ring);
			}
		}

		pool.dense.pop_back();
		pool.values.resize(pool.values.size() - size);
		if (!pool.samples.empty())
			pool.samples.resize(pool.samples.size() - static_cast<size_t>(REPLICA_SAMPLES) * size);
		pool.sparse[index] = NONE;
	}

	u8* Replica::value_of(u32 index, ComponentId component) noexcept
	{
		Pool& pool	 = m_pools[component];
		const u32 at = pool.sparse[index];
		return at == NONE ? nullptr : pool.values.data() + static_cast<size_t>(at) * m_schema.component(component).size;
	}

	const u8* Replica::value_of(u32 index, ComponentId component) const noexcept
	{
		const Pool& pool = m_pools[component];
		const u32 at	 = pool.sparse[index];
		return at == NONE ? nullptr : pool.values.data() + static_cast<size_t>(at) * m_schema.component(component).size;
	}

	u8* Replica::sample_of(u32 index, ComponentId component, u32 place) noexcept
	{
		Pool& pool	   = m_pools[component];
		const u32 size = m_schema.component(component).size;
		return pool.samples.data() + (static_cast<size_t>(pool.sparse[index]) * REPLICA_SAMPLES + place) * size;
	}

	const u8* Replica::sample_of(u32 index, ComponentId component, u32 place) const noexcept
	{
		const Pool& pool = m_pools[component];
		const u32 size	 = m_schema.component(component).size;
		return pool.samples.data() + (static_cast<size_t>(pool.sparse[index]) * REPLICA_SAMPLES + place) * size;
	}

	u32 Replica::place_of(const Entity& entity, u32 i) noexcept { return (entity.first + i) % REPLICA_SAMPLES; }

	u32 Replica::push_sample(Entity& entity, Tick tick) noexcept
	{
		// The ring is full: the oldest makes way.
		if (entity.samples == REPLICA_SAMPLES)
		{
			entity.first = static_cast<u8>((entity.first + 1) % REPLICA_SAMPLES);
			--entity.samples;
		}

		const u32 place		= place_of(entity, entity.samples);
		entity.ticks[place] = tick;
		++entity.samples;
		return place;
	}
}
