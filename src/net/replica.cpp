#include <ember/memory/memory.h>
#include <ember/net/replica.h>

#include <algorithm>

namespace ember::net
{
	Replica::Replica(ecs::World& world, const ReplicaDef& def) noexcept
		: m_world(world), m_schema(world, def.max_entities), m_pools(&memory::heap(MemoryTag::Network)),
		  m_entities(&memory::heap(MemoryTag::Network)), m_staged_removals(&memory::heap(MemoryTag::Network)),
		  m_staged(&memory::heap(MemoryTag::Network)), m_staged_values(&memory::heap(MemoryTag::Network))
	{
		EMBER_ASSERT(world.role() == ecs::Role::Client && "a replica fills a client world");

		entt::registry& registry = world.registry;
		(void)registry.storage<NetId>();
		(void)registry.storage<Owned>();

		// A pool per sampled component type, empty until entities have them.
		const ComponentMask sampled = m_schema.interpolated() | m_schema.predicted();
		m_pools.reserve(m_schema.component_count());
		for (u32 id = 0; id < m_schema.component_count(); ++id)
		{
			m_schema.component(static_cast<ComponentId>(id)).assure(registry);

			Pool& pool = m_pools.emplace_back(Pool{.sparse = Vector<u32>(&memory::heap(MemoryTag::Network)),
												   .dense  = Vector<u32>(&memory::heap(MemoryTag::Network)),
												   .values = Vector<u8>(&memory::heap(MemoryTag::Network))});
			if ((sampled & component_bit(static_cast<ComponentId>(id))) != 0)
				pool.sparse.resize(m_schema.max_entities(), NONE);
		}

		m_entities.resize(m_schema.max_entities());
	}

	void Replica::reset() noexcept
	{
		for (u32 index = 0; index < m_entities.size(); ++index)
		{
			if (m_entities[index].alive)
				remove(index);
		}

		m_latest = NO_TICK;
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

	void Replica::interpolate(f64 tick) noexcept
	{
		entt::registry& registry = m_world.registry;

		for (ComponentMask left = m_schema.interpolated(); left != 0;)
		{
			const ComponentId component	   = detail::take_lowest(left);
			const ecs::ComponentInfo& info = m_schema.component(component);
			const bool predicted		   = (m_schema.predicted() & component_bit(component)) != 0;

			for (const u32 index : m_pools[component].dense)
			{
				const Slot& slot = m_entities[index];
				if (predicted && slot.owned)
					continue; // its owner's simulation draws it

				void* out = info.get(registry, slot.entity);
				if (out == nullptr || slot.samples == 0)
					continue;

				// The newest sample at or before tick: the one to draw from.
				u32 at = 0;
				while (at + 1 < slot.samples && static_cast<f64>(slot.ticks[place_of(slot, at + 1)]) <= tick)
					++at;

				const u8* from	   = value_of(index, component, place_of(slot, at));
				const Tick start   = slot.ticks[place_of(slot, at)];
				const bool between = at + 1 < slot.samples && static_cast<f64>(start) <= tick;
				if (!between)
				{
					std::memcpy(out, from, info.size); // before the oldest or past the newest: hold it
					continue;
				}

				const Tick end = slot.ticks[place_of(slot, at + 1)];
				const f32 t	   = static_cast<f32>((tick - static_cast<f64>(start)) / static_cast<f64>(end - start));
				info.interpolate(from, value_of(index, component, place_of(slot, at + 1)), t, out);
			}
		}
	}

	ecs::Entity Replica::entity(NetId id) const noexcept
	{
		const u32 index = id.index();
		if (!id || index >= m_entities.size())
			return ecs::NO_ENTITY;

		const Slot& slot = m_entities[index];
		return slot.alive && slot.generation == id.generation() ? slot.entity : ecs::NO_ENTITY;
	}

	Tick Replica::tick(ecs::Entity entity) const noexcept
	{
		const entt::registry& registry = m_world.registry;
		const NetId* id				   = registry.valid(entity) ? registry.try_get<NetId>(entity) : nullptr;
		return id != nullptr && this->entity(*id) == entity ? m_entities[id->index()].tick : NO_TICK;
	}

	const void* Replica::newest(ecs::Entity entity, entt::id_type type) const noexcept
	{
		const entt::registry& registry = m_world.registry;
		const NetId* id				   = registry.valid(entity) ? registry.try_get<NetId>(entity) : nullptr;
		if (id == nullptr || this->entity(*id) != entity)
			return nullptr;

		for (u32 component = 0; component < m_schema.component_count(); ++component)
		{
			const Pool& pool = m_pools[component];
			if (m_schema.component(static_cast<ComponentId>(component)).type != type || pool.sparse.empty())
				continue;

			return pool.sparse[id->index()] != NONE ? value_of(id->index(), static_cast<ComponentId>(component), NEWEST)
													: nullptr;
		}

		return nullptr;
	}

	ComponentMask Replica::seen_in_prefab(PrefabId prefab, bool owned) const noexcept
	{
		const PrefabInfo& info = m_schema.prefab(prefab);
		return owned ? info.components : info.shared;
	}

	ComponentMask Replica::sampled(const Slot& slot) const noexcept
	{
		return m_schema.interpolated() | (slot.owned ? m_schema.predicted() : 0);
	}

	ComponentMask Replica::written(const Slot& slot) const noexcept
	{
		// What is drawn is written by interpolate(), and what is predicted by the owner's simulation.
		return ~sampled(slot);
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
				const Slot& slot = m_entities[staged.index];
				if (!slot.alive)
					return false;

				staged.prefab	  = slot.prefab;
				staged.generation = slot.generation;
				staged.owned	  = slot.owned;
				had				  = slot.components;
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

				const ecs::ComponentInfo& info = m_schema.component(component);
				const size_t at				   = m_staged_values.size();
				m_staged_values.resize(at + info.size);
				if (!info.read(stream, m_staged_values.data() + at))
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
		entt::registry& registry = m_world.registry;

		for (const u32 index : m_staged_removals)
		{
			if (m_entities[index].alive)
				remove(index);
		}

		for (const Staged& staged : m_staged)
		{
			Slot& slot = m_entities[staged.index];

			const bool fresh = staged.create && (!slot.alive || slot.generation != staged.generation);
			if (fresh)
			{
				if (slot.alive)
					remove(staged.index); // only a broken server reuses an index this client still has

				spawn(staged);
			}
			else if (!slot.alive)
			{
				continue; // removed by this very section: only a broken server sends that
			}

			// Its components as the record has them: a lost one goes, a gained one arrives with its value.
			// The world's entity is the client's to lose, though it should not: then only the books are kept.
			const bool present		   = registry.valid(slot.entity);
			const ComponentMask keeps  = sampled(slot);
			const ComponentMask lost   = slot.components & ~staged.components;
			const ComponentMask gained = staged.components & ~slot.components;

			for (ComponentMask left = lost; left != 0;)
			{
				const ComponentId component = detail::take_lowest(left);
				if (present)
					m_schema.component(component).remove(registry, slot.entity);
				if ((keeps & component_bit(component)) != 0)
					erase(staged.index, component);
			}

			slot.components = staged.components;

			// Values: into the world, but what is drawn or predicted, which the samples keep. A new
			// entity or component starts from its value whatever it is.
			const ComponentMask writes = fresh ? ~ComponentMask{0} : written(slot) | gained;
			u32 at					   = staged.values;
			for (ComponentMask left = staged.carried; left != 0;)
			{
				const ComponentId component	   = detail::take_lowest(left);
				const ecs::ComponentInfo& info = m_schema.component(component);
				const u8* value				   = m_staged_values.data() + at;
				at += info.size;

				if (present && (writes & component_bit(component)) != 0)
					info.emplace(registry, slot.entity, value);

				if ((keeps & component_bit(component)) == 0)
					continue;

				if ((gained & component_bit(component)) != 0)
					insert(staged.index, component, value); // drawn at its value as far back as the samples go
				else
					std::memcpy(value_of(staged.index, component, NEWEST), value, info.size);
			}

			// A new state: sample it where it began, and when the state before it is known to have held
			// until then, close that one off just before.
			const ComponentMask samples = slot.components & keeps;
			const bool newer = slot.samples == 0 || staged.changed > slot.ticks[place_of(slot, slot.samples - 1u)];
			if (samples != 0 && staged.changed != slot.since && newer)
			{
				if (slot.samples > 0 && staged.previous == slot.since &&
					staged.changed - 1 > slot.ticks[place_of(slot, slot.samples - 1u)])
				{
					const u32 last	= place_of(slot, slot.samples - 1u);
					const u32 place = push_sample(slot, staged.changed - 1);
					for (ComponentMask left = samples; left != 0;)
					{
						const ComponentId component = detail::take_lowest(left);
						std::memcpy(value_of(staged.index, component, place), value_of(staged.index, component, last),
									m_schema.component(component).size);
					}
				}

				const u32 place = push_sample(slot, staged.changed);
				for (ComponentMask left = samples; left != 0;)
				{
					const ComponentId component = detail::take_lowest(left);
					std::memcpy(value_of(staged.index, component, place), value_of(staged.index, component, NEWEST),
								m_schema.component(component).size);
				}
			}

			slot.since = staged.changed;
			slot.tick  = tick;
		}

		m_latest = tick;
	}

	void Replica::spawn(const Staged& staged) noexcept
	{
		entt::registry& registry  = m_world.registry;
		const ecs::Prefab& prefab = m_world.prefabs()[staged.prefab];

		Slot& slot		= m_entities[staged.index];
		slot			= {};
		slot.prefab		= staged.prefab;
		slot.generation = staged.generation;
		slot.alive		= true;
		slot.owned		= staged.owned;

		// The client's half of its prefab, and what the client predicts. Another seat's OwnerOnly
		// components never come, so this client has none of them.
		slot.entity = m_world.create(prefab, staged.owned);
		if (!staged.owned)
		{
			for (ComponentMask left = m_schema.prefab(staged.prefab).components & m_schema.owner_only(); left != 0;)
				m_schema.component(detail::take_lowest(left)).remove(registry, slot.entity);
		}

		registry.emplace<NetId>(slot.entity, NetId::make(staged.index, staged.generation));
		if (staged.owned)
			registry.emplace<Owned>(slot.entity, Owned{.key = staged.key});

		// A record carries only what differs from the prefab's values.
		slot.components = seen_in_prefab(staged.prefab, staged.owned);
		for (ComponentMask left = slot.components & sampled(slot); left != 0;)
		{
			const ComponentId component = detail::take_lowest(left);
			insert(staged.index, component, prefab.find(m_schema.component(component).id)->value.data());
		}

		++m_alive;
	}

	void Replica::remove(u32 index) noexcept
	{
		Slot& slot = m_entities[index];
		for (ComponentMask left = slot.components & sampled(slot); left != 0;)
			erase(index, detail::take_lowest(left));

		if (m_world.registry.valid(slot.entity))
			m_world.registry.destroy(slot.entity);

		slot.entity		= ecs::NO_ENTITY;
		slot.components = 0;
		slot.alive		= false;
		--m_alive;
	}

	void Replica::insert(u32 index, ComponentId component, const void* value) noexcept
	{
		Pool& pool	   = m_pools[component];
		const u32 size = m_schema.component(component).size;
		EMBER_ASSERT(!pool.sparse.empty() && pool.sparse[index] == NONE);

		pool.sparse[index] = static_cast<u32>(pool.dense.size());
		pool.dense.push_back(index);
		pool.values.resize(pool.values.size() + static_cast<size_t>(NEWEST + 1) * size);

		for (u32 place = 0; place <= NEWEST; ++place)
			std::memcpy(value_of(index, component, place), value, size);
	}

	void Replica::erase(u32 index, ComponentId component) noexcept
	{
		// The last one takes the place of the one that goes, so the pool stays packed.
		Pool& pool		  = m_pools[component];
		const size_t span = static_cast<size_t>(NEWEST + 1) * m_schema.component(component).size;
		const u32 at	  = pool.sparse[index];
		EMBER_ASSERT(at != NONE);

		const u32 last = static_cast<u32>(pool.dense.size() - 1);
		if (at != last)
		{
			pool.dense[at]				= pool.dense[last];
			pool.sparse[pool.dense[at]] = at;
			std::memcpy(pool.values.data() + at * span, pool.values.data() + last * span, span);
		}

		pool.dense.pop_back();
		pool.values.resize(pool.values.size() - span);
		pool.sparse[index] = NONE;
	}

	u8* Replica::value_of(u32 index, ComponentId component, u32 place) noexcept
	{
		Pool& pool	   = m_pools[component];
		const u32 size = m_schema.component(component).size;
		return pool.values.data() + (static_cast<size_t>(pool.sparse[index]) * (NEWEST + 1) + place) * size;
	}

	const u8* Replica::value_of(u32 index, ComponentId component, u32 place) const noexcept
	{
		const Pool& pool = m_pools[component];
		const u32 size	 = m_schema.component(component).size;
		return pool.values.data() + (static_cast<size_t>(pool.sparse[index]) * (NEWEST + 1) + place) * size;
	}

	u32 Replica::place_of(const Slot& slot, u32 i) noexcept { return (slot.first + i) % REPLICA_SAMPLES; }

	u32 Replica::push_sample(Slot& slot, Tick tick) noexcept
	{
		// The ring is full: the oldest makes way.
		if (slot.samples == REPLICA_SAMPLES)
		{
			slot.first = static_cast<u8>((slot.first + 1) % REPLICA_SAMPLES);
			--slot.samples;
		}

		const u32 place	  = place_of(slot, slot.samples);
		slot.ticks[place] = tick;
		++slot.samples;
		return place;
	}
}
