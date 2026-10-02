#include <ember/memory/memory.h>
#include <ember/net/replication.h>

#include <algorithm>

namespace ember::net
{
	namespace
	{
		/** A component's wire form when written `offset` bits into a stream. */
		[[nodiscard]] u32 written_bits(const ComponentCodec& codec, const void* value, u32 offset,
									   ComponentBits* out) noexcept
		{
			PacketBuffer scratch;
			serialize::WriteStream stream = packet_writer(scratch);

			u32 zero = 0;
			if (offset > 0)
				stream.SerializeBits(zero, static_cast<int>(offset));

			[[maybe_unused]] const bool wrote = codec.write(stream, value);
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
	}

	Schema::Schema(u32 max_entities) noexcept
		: m_max_entities(max_entities), m_components(&memory::heap(MemoryTag::Network)),
		  m_prefabs(&memory::heap(MemoryTag::Network)), m_values(&memory::heap(MemoryTag::Network))
	{
		EMBER_ASSERT(max_entities >= 2 && max_entities <= (1u << NetId::INDEX_BITS));

		m_index_bits = static_cast<u32>(serialize::bits_required(0, max_entities - 1));
		m_components.reserve(MAX_COMPONENT_TYPES);
	}

	ComponentId Schema::add_component(const ComponentCodec& codec) noexcept
	{
		EMBER_ASSERT(m_components.size() < MAX_COMPONENT_TYPES);
		EMBER_ASSERT(m_prefabs.empty() && "add every component type before the first prefab");
		EMBER_ASSERT(codec.size > 0 && codec.size <= MAX_COMPONENT_BYTES);
		EMBER_ASSERT(codec.write != nullptr && codec.read != nullptr);

		[[maybe_unused]] const u32 bits = written_bits(codec, codec.empty.data(), 0, nullptr);
		EMBER_ASSERT(bits <= MAX_COMPONENT_BITS && "a component's wire form is MAX_COMPONENT_BITS at most");

		// Packets take a component's bits wherever they have got to. One that aligns would pad
		// differently there than where it was written, and every bit after it would shift.
		EMBER_ASSERT(written_bits(codec, codec.empty.data(), 1, nullptr) == bits &&
					 "replicated components must not align: no serialize_bytes, serialize_string or serialize_align");

		const ComponentId id = static_cast<ComponentId>(m_components.size());
		if (has_any(codec.flags, ComponentFlags::OwnerOnly))
			m_owner_only |= component_bit(id);

		m_components.push_back(codec);
		return id;
	}

	PrefabId Schema::add_prefab(Span<const PrefabComponent> components, f32 priority) noexcept
	{
		EMBER_ASSERT(m_prefabs.size() < 0xffff);
		EMBER_ASSERT(priority > 0.0f);

		PrefabInfo info;
		info.priority = priority;
		info.first	  = static_cast<u32>(m_values.size());

		for (const PrefabComponent& component : components)
		{
			EMBER_ASSERT(component.component < m_components.size() && "add the component types first");
			EMBER_ASSERT((info.components & component_bit(component.component)) == 0 && "a component twice");
			info.components |= component_bit(component.component);
		}
		info.shared = info.components & ~m_owner_only;

		// One value per component, in id order, whatever order the game listed them in.
		for (ComponentMask left = info.components; left != 0;)
		{
			const ComponentId id		= detail::take_lowest(left);
			const ComponentCodec& codec = m_components[id];

			const void* value = codec.empty.data();
			for (const PrefabComponent& component : components)
			{
				if (component.component == id && component.value != nullptr)
					value = component.value;
			}

			// The value as clients will decode it: what the prefab's bits come back as.
			PrefabValue& stored				= m_values.emplace_back();
			[[maybe_unused]] const u32 bits = written_bits(codec, value, 0, &stored.wire);
			EMBER_ASSERT(bits <= MAX_COMPONENT_BITS && "a component's wire form is MAX_COMPONENT_BITS at most");

			serialize::ReadStream reader(stored.wire.bytes.data(), (stored.wire.bits + 7) / 8);
			[[maybe_unused]] const bool read = codec.read(reader, stored.bytes.data());
			EMBER_ASSERT(read && "a prefab's value does not read back");
		}

		m_prefabs.push_back(info);
		return static_cast<PrefabId>(m_prefabs.size() - 1);
	}

	const PrefabValue& Schema::prefab_value(PrefabId id, ComponentId component) const noexcept
	{
		const PrefabInfo& info = m_prefabs[id];
		EMBER_ASSERT((info.components & component_bit(component)) != 0 && "the prefab has no such component");

		const u32 rank = detail::component_count(info.components & (component_bit(component) - 1));
		return m_values[info.first + rank];
	}

	u32 Schema::prefab_bits() const noexcept
	{
		return m_prefabs.size() < 2 ? 0 : static_cast<u32>(serialize::bits_required(0, prefab_count() - 1));
	}

	u32 Schema::component_bits() const noexcept
	{
		return m_components.size() < 2 ? 0 : static_cast<u32>(serialize::bits_required(0, component_count() - 1));
	}
}
