#pragma once

#include <ember/containers/span.h>
#include <ember/core/bitmask.h>
#include <ember/net/serialize.h>
#include <ember/net/tick.h>

#include <array>
#include <bit>
#include <cstring>
#include <type_traits>

namespace ember::net
{
	/** Component types a schema holds at most: one bit each in ComponentMask */
	inline constexpr u32 MAX_COMPONENT_TYPES = 64;

	/** The largest replicated component: in memory, and on the wire. */
	inline constexpr u32 MAX_COMPONENT_BYTES = 64;
	inline constexpr u32 MAX_COMPONENT_BITS	 = 512;

	/**
	 * A replicated entity's name on every machine in a session: its index in the server's entity
	 * table and the generation of that index. A destroyed entity's index is only reused once every
	 * client has let go of it, and then with the next generation, so an ID kept past its entity's
	 * death (in a command, a message, the game's own tables) is told apart form the entity now in
	 * place.
	 */
	struct NetId
	{
		static constexpr u32 INDEX_BITS		 = 20;
		static constexpr u32 GENERATION_BITS = 12;
		static constexpr u32 MAX_GENERATION	 = (1u << GENERATION_BITS) - 1;

		u32 value = 0; // 0 is no entity: generations start at 1

		[[nodiscard]] static constexpr NetId make(u32 index, u32 generation) noexcept
		{
			return {(generation << INDEX_BITS) | index};
		}

		[[nodiscard]] constexpr u32 index() const noexcept { return value & ((1u << INDEX_BITS) - 1); }
		[[nodiscard]] constexpr u32 generation() const noexcept { return value >> INDEX_BITS; }
		[[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
		constexpr bool operator==(const NetId&) const noexcept = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, value, 32);
			return true;
		}
	};

	inline constexpr NetId NO_NET_ID = {};

	/** A component type's place in a schema. */
	using ComponentId = u8;

	inline constexpr ComponentId NO_COMPONENT = 0xff;

	/** Component types, a bit each by id: what an entity has, what a record carries. */
	using ComponentMask = u64;

	static_assert(MAX_COMPONENT_TYPES == sizeof(ComponentMask) * 8, "one bit per component type");

	constexpr ComponentMask component_bit(ComponentId id) noexcept { return ComponentMask{1} << id; }

	/** A prefab's place in a schema: what the game spawns from, a player, an enemy, etc. */
	using PrefabId = u16;

	enum class ComponentFlags : u8
	{
		None		 = 0,
		Interpolated = 1 << 0, // clients keep timed samples of it, to draw it between server ticks
		OwnerOnly	 = 1 << 1, // only the entity's owner receives it: ammo, cooldowns, private state.
	};

	EMBER_ENUM_BITWISE_OPS(ComponentFlags, u8);

	/**
	 * A component type as replication sees it: bytes, how they go on the wire, and how they are treated.
	 * component_codec<T>() builds one from T's serialize(), as command_codec does for commands, so
	 * replication itself never names a gameplay type.
	 *
	 * The server keeps every component as the bits its serialize() wrote and copies those bits into
	 * each packet wherever the packet has got to, so a component must not align: no serialize_bytes,
	 * serialize_string, or serialize_algn. Pack with serialize_bts, serialize_int, and the compressed
	 * floats. Schema::add_component checks this once.
	 */
	struct ComponentCodec
	{
		u32 size			 = 0;		// sizeof the component: trivially copyable, at most MAX_COMPONENT_BYTES
		const void* type	 = nullptr; // which type it was made for, so typed calls can check what they are given.
		ComponentFlags flags = ComponentFlags::None;

		bool (*write)(serialize::WriteStream& stream, const void* value) noexcept = nullptr;
		bool (*read)(serialize::ReadStream& stream, void* value) noexcept		  = nullptr;

		/** A default constructed component: a prefab's value for it when the prefab does not say. */
		std::array<u8, MAX_COMPONENT_BYTES> empty = {};
	};

	namespace detail
	{
		/** One address per component type: what ComponentCodec::type points at. */
		template <class T> inline constexpr char COMPONENT_TYPE = 0;

		/** A component copied out of storage, which is bytes, into a value of its own type. */
		template <class T> [[nodiscard]] T load_component(const void* bytes) noexcept
		{
			T value{};
			std::memcpy(&value, bytes, sizeof(T));
			return value;
		}
	}

	template <class T>
	[[nodiscard]] ComponentCodec component_codec(ComponentFlags flags = ComponentFlags::None) noexcept
	{
		static_assert(std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>,
					  "components are plain values: copied as bytes, default constructed until set");
		static_assert(sizeof(T) <= MAX_COMPONENT_BYTES, "components are small: split a large one");

		ComponentCodec codec;
		codec.size	= sizeof(T);
		codec.type	= &detail::COMPONENT_TYPE<T>;
		codec.flags = flags;

		const T empty{};
		std::memcpy(codec.empty.data(), &empty, sizeof(T));

		codec.write = [](serialize::WriteStream& stream, const void* value) noexcept
		{
			T copy = detail::load_component<T>(value);
			return copy.serialize(stream);
		};

		codec.read = [](serialize::ReadStream& stream, void* value) noexcept
		{
			T copy{};
			if (!copy.serialize(stream))
				return false;

			std::memcpy(value, &copy, sizeof(T));
			return true;
		};

		return codec;
	}

	/** A component's wire form: the bits its serialize() wrote, ready to copy into a packet. */
	struct ComponentBits
	{
		u32 bits													= 0;
		alignas(8) std::array<u8, MAX_COMPONENT_BITS / 8 + 8> bytes = {}; // + 8: the bit reader's slack
	};

	/** One replicated component of a prefab, as the game's prefab file sets it. */
	struct PrefabComponent
	{
		ComponentId component = 0;
		const void* value	  = nullptr; // a value of the component's type; null for its default
	};

	/**
	 * A prefab as replication sees it: the replicated components an entity made from it starts with,
	 * and how urgent its updates are. Both ends of a session know every prefab and its values, so a
	 * create names the prefab and carries only what differs from it.
	 */
	struct PrefabInfo
	{
		ComponentMask components = 0;	 // what a new entity has
		ComponentMask shared	 = 0;	 // what a viewer that does not own it gets: less the OwnerOnly ones
		f32 priority			 = 1.0f; // multiplies the game's relevance: a volley or a boss outranks a crate
		u32 first				 = 0;	 // its first value in the schema; one per component, in id order
	};

	/** A prefab's value for one of its components: as clients decode it, and the bits that decode to it. */
	struct PrefabValue
	{
		ComponentBits wire;
		std::array<u8, MAX_COMPONENT_BYTES> bytes = {};
	};

	/**
	 * What both ends of a session replicate: the component types, the prefabs entities are made from,
	 * and how many entities exist at once. The server and its clients build the same schema, in the
	 * same order, from shared game code and the same prefab files; a change to it is a change to the
	 * game's protocol version, which the handshake checks.
	 */
	class Schema final
	{
	public:
		/** max_entities sets the width of every entity index on the wire, so both ends must agree on it. */
		explicit Schema(u32 max_entities = 4096) noexcept;

		/** A component type; ids follow the order of registration. */
		template <class T> ComponentId add_component(ComponentFlags flags = ComponentFlags::None) noexcept
		{
			return add_component(component_codec<T>(flags));
		}

		ComponentId add_component(const ComponentCodec& codec) noexcept;

		/**
		 * A prefab: the replicated components an entity made from it starts with, at these values, in
		 * any order. Ids follow the order of registration. Each value is kept as clients decode it,
		 * so the server's copy and every client's agree to the bit.
		 */
		PrefabId add_prefab(Span<const PrefabComponent> components, f32 priority = 1.0f) noexcept;

		/** T's component id; NO_COMPONENT when T was never added. */
		template <class T> ComponentId id_of() const noexcept
		{
			for (u32 id = 0; id < m_components.size(); ++id)
			{
				if (m_components[id].type == &detail::COMPONENT_TYPE<T>)
					return static_cast<ComponentId>(id);
			}

			return NO_COMPONENT;
		}

		u32 max_entities() const noexcept { return m_max_entities; }
		u32 component_count() const noexcept { return static_cast<u32>(m_components.size()); }
		u32 prefab_count() const noexcept { return static_cast<u32>(m_prefabs.size()); }

		const ComponentCodec& component(ComponentId id) const noexcept { return m_components[id]; }
		const PrefabInfo& prefab(PrefabId id) const noexcept { return m_prefabs[id]; }

		/** A prefab's value for one of its components. */
		const PrefabValue& prefab_value(PrefabId id, ComponentId component) const noexcept;

		/** The component types only an entity's owner receives. */
		[[nodiscard]] ComponentMask owner_only() const noexcept { return m_owner_only; }

		/** Bits on the wire of an entity index, a prefab id and a component id. */
		[[nodiscard]] u32 index_bits() const noexcept { return m_index_bits; }
		[[nodiscard]] u32 prefab_bits() const noexcept;
		[[nodiscard]] u32 component_bits() const noexcept;

	private:
		u32 m_max_entities		   = 0;
		u32 m_index_bits		   = 0;
		ComponentMask m_owner_only = 0;

		Vector<ComponentCodec> m_components;
		Vector<PrefabInfo> m_prefabs;
		Vector<PrefabValue> m_values; // every prefab's, a run per prefab
	};

	/**
	 * The entity section of the server's packets, after the timing report. The endpoints write and
	 * read the first bit; Replicator::write and Replica::read the rest.
	 *
	 *   present      1   a section follows; a server without a Replicator sends 0
	 *   tick        32   the server tick every record describes
	 *   removals    10   count, then each removed entity's index
	 *   records     10   count, then each record:
	 *     index           schema.index_bits()
	 *     create       1  then: prefab (schema.prefab_bits()), generation 12, owned 1, key present 1 [, key 32]
	 *     age     varint  ticks since the entity's state last changed
	 *     previous     1  then varint: ticks between that change and the one before it, less one
	 *     set          1  then the components that differ from the prefab's, as this viewer gets them:
	 *                     a varint count, then each one's id (schema.component_bits()), ascending
	 *     mask         n  which of the entity's n components follow, lowest id first
	 *     components      each one's bits, as its serialize() wrote them
	 *
	 * A record carries every component that changed since the newest  state the client is known to have,
	 * and the entity's component set whenever that changed too, so applying it leaves the client's copy
	 * exactly the server's entity at `tick`. A set is told as the components that differ from the prefab's,
	 * never from an earlier set, so any one record says it whole.
	 */
	inline constexpr u32 SECTION_COUNT_BITS = 10;
	inline constexpr u32 MAX_SECTION_ITEMS	= (1u << SECTION_COUNT_BITS) - 1;

	namespace detail
	{
		/**
		 * An unsigned number that is usually small: 0 in 1 bit, 1 to 16 in 6, 17 to 272 in 11, the
		 * rest in 35.
		 */
		template <class Stream> bool serialize_varint(Stream& stream, u32& value)
		{
			bool more = Stream::IsWriting && value > 0;
			serialize_bool(stream, more);
			if (!more)
			{
				if (Stream::IsReading)
					value = 0;
				return true;
			}

			bool wide = Stream::IsWriting && value > 16;
			serialize_bool(stream, wide);
			if (!wide)
			{
				u32 low = Stream::IsWriting ? value - 1 : 0;
				serialize_bits(stream, low, 4);
				if (Stream::IsReading)
					value = low + 1;
				return true;
			}

			bool widest = Stream::IsWriting && value > 272;
			serialize_bool(stream, widest);
			if (!widest)
			{
				u32 low = Stream::IsWriting ? value - 17 : 0;
				serialize_bits(stream, low, 8);
				if (Stream::IsReading)
					value = low + 17;
				return true;
			}

			serialize_bits(stream, value, 32);
			return true;
		}

		[[nodiscard]] constexpr u32 varint_bits(u32 value) noexcept
		{
			return value == 0 ? 1 : value <= 16 ? 6 : value <= 272 ? 11 : 35;
		}

		/** The lowest component in a mask, taken out of it. The mask must not be empty. */
		[[nodiscard]] inline ComponentId take_lowest(ComponentMask& mask) noexcept
		{
			const ComponentId id = static_cast<ComponentId>(std::countr_zero(mask));
			mask &= mask - 1;
			return id;
		}

		/** How many components a mask holds. */
		[[nodiscard]] constexpr u32 component_count(ComponentMask mask) noexcept
		{
			return static_cast<u32>(std::popcount(mask));
		}

		/**
		 * A record's mask: bit k says the k-th of the entity's components, lowest id first, follows.
		 * Up to 64 bits, so it goes in two parts.
		 */
		template <class Stream> bool serialize_mask(Stream& stream, ComponentMask& mask, u32 bits)
		{
			u32 low	 = Stream::IsWriting ? static_cast<u32>(mask) : 0;
			u32 high = Stream::IsWriting ? static_cast<u32>(mask >> 32) : 0;
			if (bits > 0)
				serialize_bits(stream, low, bits < 32 ? bits : 32);
			if (bits > 32)
				serialize_bits(stream, high, bits - 32);

			if (Stream::IsReading)
				mask = (ComponentMask{high} << 32) | low;
			return true;
		}
	}
}
