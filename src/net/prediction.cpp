#include <ember/memory/memory.h>
#include <ember/net/prediction.h>

#include <algorithm>

namespace ember::net
{
	Prediction::Prediction(ecs::World& world, const Replica& replica) noexcept
		: m_world(world), m_replica(replica), m_types(&memory::heap(MemoryTag::Network)),
		  m_offsets(&memory::heap(MemoryTag::Network)), m_tracks(&memory::heap(MemoryTag::Network))
	{
		EMBER_ASSERT(world.role() == ecs::Role::Client && "prediction is a client's");

		for (const ecs::ComponentInfo& info : world.components().all())
		{
			if (!has_any(info.kind, ecs::Kind::Predicted))
				continue;

			m_types.push_back(&info);
			m_offsets.push_back(m_stride);
			m_stride += info.size;
		}

		EMBER_ASSERT(m_types.size() <= MAX_COMPONENT_TYPES);
	}

	void Prediction::record(Tick tick) noexcept
	{
		EMBER_ASSERT(tick != NO_TICK);

		sync();
		for (Track& track : m_tracks)
			keep(track, tick);
	}

	Tick Prediction::check() noexcept
	{
		sync();

		// The server's newest word on each owned entity, against what was predicted for that tick.
		Tick from = NO_TICK;
		for (Track& track : m_tracks)
		{
			const Tick tick = m_replica.tick(track.entity);
			if (tick == NO_TICK || (track.checked != NO_TICK && tick <= track.checked))
				continue;

			track.checked = tick;
			if (!matches(track, tick))
				from = from == NO_TICK ? tick : std::min(from, tick);
		}

		if (from == NO_TICK)
			return NO_TICK;

		// The replay runs every simulated entity, so every one goes back to that tick, not just those
		// that missed.
		for (Track& track : m_tracks)
			restore(track, from);

		++m_corrections;
		return from;
	}

	void Prediction::sync() noexcept
	{
		entt::registry& registry = m_world.registry;

		std::erase_if(m_tracks, [&](const Track& track)
					  { return !registry.valid(track.entity) || !registry.all_of<Owned>(track.entity); });

		for (const ecs::Entity entity : registry.view<const Owned, const NetId>())
		{
			const bool tracked = std::any_of(m_tracks.begin(), m_tracks.end(),
											 [entity](const Track& track) { return track.entity == entity; });
			if (tracked)
				continue;

			Track& track = m_tracks.emplace_back(Track{.entity	= entity,
													   .checked = NO_TICK,
													   .present = {},
													   .values	= Vector<u8>(&memory::heap(MemoryTag::Network))});
			track.values.resize(static_cast<size_t>(PREDICTION_HISTORY) * m_stride);
		}
	}

	void Prediction::keep(Track& track, Tick tick) noexcept
	{
		ComponentMask& present = track.present.write(tick);
		u8* values			   = row(track, tick);
		present				   = 0;

		for (u32 i = 0; i < m_types.size(); ++i)
		{
			const ecs::ComponentInfo& info = *m_types[i];
			void* value					   = info.get(m_world.registry, track.entity);
			if (value == nullptr)
				continue;

			snap(info, value);
			std::memcpy(values + m_offsets[i], value, info.size);
			present |= ComponentMask{1} << i;
		}
	}

	bool Prediction::matches(const Track& track, Tick tick) noexcept
	{
		// Nothing predicted for that tick: the entity is new to this client, or older than the history.
		const ComponentMask* present = track.present.find(tick);
		if (present == nullptr)
			return false;

		const u8* values = row(track, tick);
		for (u32 i = 0; i < m_types.size(); ++i)
		{
			const ecs::ComponentInfo& info = *m_types[i];
			const void* server			   = m_replica.server_value(track.entity, info);
			const bool predicted		   = (*present & (ComponentMask{1} << i)) != 0;

			// A component gained or lost is the server's call, and a miss until the prediction has it too.
			if ((server != nullptr) != predicted)
				return false;
			if (server == nullptr)
				continue;

			// At wire precision: a value the server never sent is still its prefab's, off the grid.
			const u32 bits = wire_form(info, server, 0);
			if (wire_form(info, values + m_offsets[i], 1) != bits ||
				std::memcmp(m_scratch[0].bytes.data(), m_scratch[1].bytes.data(), (bits + 7) / 8) != 0)
				return false;
		}

		return true;
	}

	void Prediction::restore(Track& track, Tick tick) noexcept
	{
		entt::registry& registry = m_world.registry;

		if (m_replica.tick(track.entity) == tick)
		{
			for (const ecs::ComponentInfo* info : m_types)
			{
				const void* server = m_replica.server_value(track.entity, *info);
				void* value		   = info->get(registry, track.entity);
				if (server != nullptr && value != nullptr)
					std::memcpy(value, server, info->size);
			}
		}
		else if (const ComponentMask* present = track.present.find(tick))
		{
			const u8* values = row(track, tick);
			for (u32 i = 0; i < m_types.size(); ++i)
			{
				void* value = m_types[i]->get(registry, track.entity);
				if (value != nullptr && (*present & (ComponentMask{1} << i)) != 0)
					std::memcpy(value, values + m_offsets[i], m_types[i]->size);
			}
		}

		// Snapped, a value the server never sent included, it is the prediction for tick: the replay goes
		// on from here.
		keep(track, tick);
	}

	u32 Prediction::wire_form(const ecs::ComponentInfo& info, const void* value, u32 scratch) noexcept
	{
		serialize::WriteStream stream = packet_writer(m_scratch[scratch]);

		[[maybe_unused]] const bool wrote = info.write(stream, value);
		EMBER_ASSERT(wrote);
		stream.Flush();

		return static_cast<u32>(stream.GetBitsProcessed());
	}

	void Prediction::snap(const ecs::ComponentInfo& info, void* value) noexcept
	{
		const u32 bits = wire_form(info, value, 0);

		serialize::ReadStream reader(m_scratch[0].bytes.data(), static_cast<int>((bits + 7) / 8));
		[[maybe_unused]] const bool read = info.read(reader, value);
		EMBER_ASSERT(read && "a component's own bits do not read back");
	}

	u8* Prediction::row(Track& track, Tick tick) const noexcept
	{
		return track.values.data() + static_cast<size_t>(tick % PREDICTION_HISTORY) * m_stride;
	}

	const u8* Prediction::row(const Track& track, Tick tick) const noexcept
	{
		return track.values.data() + static_cast<size_t>(tick % PREDICTION_HISTORY) * m_stride;
	}
}
