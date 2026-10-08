#include <ember/memory/memory.h>
#include <ember/net/replication.h>

namespace ember::net
{
	namespace
	{
		/** A component's wire form when written `offset` bits into a stream. */
		[[nodiscard]] u32 written_bits(const ecs::ComponentInfo& info, const void* value, u32 offset,
									   ComponentBits* out) noexcept
		{
			PacketBuffer scratch;
			serialize::WriteStream stream = packet_writer(scratch);

			u32 zero = 0;
			if (offset > 0)
				stream.SerializeBits(zero, static_cast<int>(offset));

			[[maybe_unused]] const bool wrote = info.write(info, stream, value);
			EMBER_ASSERT(wrote);
			stream.Flush();

			const u32 bits = static_cast<u32>(stream.GetBitsProcessed()) - offset;
			if (out != nullptr && bits <= MAX_COMPONENT_BITS)
			{
				out->bits = bits;
				std::memcpy(out->bytes.data(), scratch.bytes.data(), (bits + 7) / 8);
			}

			return bits;
		}

		[[nodiscard]] u32 bits_for(u32 count) noexcept
		{
			return count < 2 ? 0 : static_cast<u32>(serialize::bits_required(0, count - 1));
		}
	}

	Schema::Schema(const ecs::World& world, u32 max_entities) noexcept
		: m_max_entities(max_entities), m_components(&memory::heap(MemoryTag::Network)),
		  m_prefabs(&memory::heap(MemoryTag::Network)), m_wires(&memory::heap(MemoryTag::Network))
	{
		EMBER_ASSERT(max_entities >= 2 && max_entities <= (1u << NetId::INDEX_BITS));
		m_index_bits = static_cast<u32>(serialize::bits_required(0, max_entities - 1));

		// The Replicated components, in the order the registry has them.
		const ecs::Components& components = world.components();
		Vector<ComponentId> net_ids(components.count(), NO_COMPONENT, &memory::heap(MemoryTag::Network));
		for (const ecs::ComponentInfo& info : components.all())
		{
			if (!has_any(info.kind, ecs::Kind::Replicated))
				continue;

			EMBER_ASSERT(m_components.size() < MAX_COMPONENT_TYPES &&
						 "more Replicated components than the wire numbers");

			[[maybe_unused]] const u32 bits = written_bits(info, info.defaults.data(), 0, nullptr);
			EMBER_ASSERT(bits <= MAX_COMPONENT_BITS && "a component's wire form is MAX_COMPONENT_BITS at most");

			// Packets take a component's bits wherever they have got to. One that aligns would pad
			// differently there than where it was written, and every bit after it would shift.
			EMBER_ASSERT(
				written_bits(info, info.defaults.data(), 1, nullptr) == bits &&
				"replicated components must not align: no serialize_bytes, serialize_string or serialize_align");

			const ComponentId id = static_cast<ComponentId>(m_components.size());
			if (has_any(info.kind, ecs::Kind::OwnerOnly))
				m_owner_only |= component_bit(id);
			if (has_any(info.kind, ecs::Kind::Interpolated))
				m_interpolated |= component_bit(id);
			if (has_any(info.kind, ecs::Kind::Predicted))
				m_predicted |= component_bit(id);

			net_ids[info.id] = id;
			m_components.push_back(&info);
		}

		// Every prefab, in registration order: one with nothing Replicated never replicates.
		const ecs::Prefabs& prefabs = world.prefabs();
		EMBER_ASSERT(prefabs.count() <= 0xffff && "more prefabs than the wire numbers");

		m_prefabs.reserve(prefabs.count());
		for (u32 id = 0; id < prefabs.count(); ++id)
		{
			const ecs::Prefab& prefab = prefabs[id];
			PrefabInfo& info		  = m_prefabs.emplace_back();
			info.first				  = static_cast<u32>(m_wires.size());

			for (const ecs::PrefabComponent& component : prefab.components)
			{
				if (net_ids[component.id] != NO_COMPONENT)
					info.components |= component_bit(net_ids[component.id]);
			}
			info.shared = info.components & ~m_owner_only;

			// One value per component, in id order, whatever order the prefab lists them in.
			for (ComponentMask left = info.components; left != 0;)
			{
				const ComponentId net_id	   = detail::take_lowest(left);
				const ecs::ComponentInfo& type = *m_components[net_id];

				[[maybe_unused]] const u32 bits =
					written_bits(type, prefab.find(type.id)->value.data(), 0, &m_wires.emplace_back());
				EMBER_ASSERT(bits <= MAX_COMPONENT_BITS && "a component's wire form is MAX_COMPONENT_BITS at most");
			}
		}

		m_prefab_bits	 = bits_for(prefab_count());
		m_component_bits = bits_for(component_count());
	}

	void Schema::retune(PrefabId id, const ecs::Prefab& prefab) noexcept
	{
		const PrefabInfo& info = m_prefabs[id];
		u32 at				   = info.first;
		for (ComponentMask left = info.components; left != 0; ++at)
		{
			const ecs::ComponentInfo& type	  = *m_components[detail::take_lowest(left)];
			const ecs::PrefabComponent* value = prefab.find(type.id);
			EMBER_ASSERT(value != nullptr && "a retuned prefab keeps its components");
			if (value != nullptr)
				(void)written_bits(type, value->value.data(), 0, &m_wires[at]);
		}
	}

	const ComponentBits& Schema::prefab_wire(PrefabId id, ComponentId component) const noexcept
	{
		const PrefabInfo& info = m_prefabs[id];
		EMBER_ASSERT((info.components & component_bit(component)) != 0 && "the prefab has no such component");

		const u32 rank = detail::component_count(info.components & (component_bit(component) - 1));
		return m_wires[info.first + rank];
	}
}
