#pragma once

#include <ember/core/bitmask.h>
#include <ember/ecs/world.h>
#include <ember/net/serialize.h>
#include <ember/net/tick.h>

#include <array>
#include <bit>

namespace ember::net
{
	/** Replicated component types a game has at most: one bit each in ComponentMask. */
	inline constexpr u32 MAX_COMPONENT_TYPES = 64;

	/** The largest Replicated component on the wire. */
	inline constexpr u32 MAX_COMPONENT_BITS = ecs::MAX_REPLICATED_BITS;

	/**
	 * A replicated entity's name on every machine in a session: its index in the server's entity
	 * table and the generation of that index. A destroyed entity's index is only reused once every
	 * client has let go of it, and then with the next generation, so an ID kept past its entity's
	 * death (in a command, a message, the game's own tables) is told apart from the entity now in
	 * place.
	 *
	 * Every replicated entity has one as a component, on the server and on its clients: what a
	 * command names an entity by.
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
	EMBER_COMPONENT(NetId, Sim);

	inline constexpr NetId NO_NET_ID = {};

	/** An entity no client controls. */
	inline constexpr u8 NO_OWNER = 0xff;

	/**
	 * On the server: the seat that controls an entity, which alone receives its OwnerOnly components
	 * and predicts it. Give it in the spawn, commands.spawn(PLAYER, net::Owner{.seat = seat}); it
	 * holds for as long as that seat's session lasts.
	 */
	struct Owner
	{
		u8 seat = NO_OWNER;
		u32 key = 0; // the game's tag, sent only to the owner: matches a spawn it predicted, a shot it fired
	};
	EMBER_COMPONENT(Owner, Server);

	/** On a client: an entity this client controls, and so predicts: it is Simulated there. */
	struct Owned
	{
		u32 key = 0; // the Owner key the server spawned it with
	};
	EMBER_COMPONENT(Owned, Client);

	/**
	 * On the server: how urgent an entity's updates are, times the game's relevance for each viewer.
	 * 1 without one; a boss or a volley more, a crate less. Put it in a prefab, or change it as the
	 * entity plays.
	 */
	struct Priority
	{
		f32 value = 1.0f;
	};
	EMBER_COMPONENT(Priority, Server);

	/** A Replicated component type's place among a game's Replicated components. */
	using ComponentId = u8;

	inline constexpr ComponentId NO_COMPONENT = 0xff;

	/** Component types, a bit each by id: what an entity has, what a record carries. */
	using ComponentMask = u64;

	static_assert(MAX_COMPONENT_TYPES == sizeof(ComponentMask) * 8, "one bit per component type");

	constexpr ComponentMask component_bit(ComponentId id) noexcept { return ComponentMask{1} << id; }

	/** A prefab's place on the wire: its place in the game's registry. */
	using PrefabId = u16;

	/** A component's wire form: the bits its serialize() wrote, ready to copy into a packet. */
	struct ComponentBits
	{
		u32 bits													= 0;
		alignas(8) std::array<u8, MAX_COMPONENT_BITS / 8 + 8> bytes = {}; // + 8: the bit reader's slack
	};

	/** A prefab as replication sees it: the Replicated components an entity made from it starts with. */
	struct PrefabInfo
	{
		ComponentMask components = 0; // what a new entity has
		ComponentMask shared	 = 0; // what a viewer that does not own it gets: less the OwnerOnly ones
		u32 first				 = 0; // its first wire value in the schema; one per component, in id order
	};

	/**
	 * What both ends of a session replicate, read from the world's registry: its Replicated
	 * components in the order they were registered, its prefabs, and how many entities exist at once.
	 * The server and its clients register the same things in the same order from shared game code, so
	 * they build the same schema; a change to it is a change to the game's protocol version, which
	 * the handshake checks. Replicator and Replica each build their own.
	 *
	 * The server keeps every component as the bits its serialize() wrote and copies those bits into
	 * each packet wherever the packet has got to, so a component must not align: no serialize_bytes,
	 * serialize_string or serialize_align. The schema checks this once.
	 */
	class Schema final
	{
	public:
		/** max_entities sets the width of every entity index on the wire, so both ends must agree on it. */
		Schema(const ecs::World& world, u32 max_entities) noexcept;

		Schema(const Schema&)			 = delete;
		Schema& operator=(const Schema&) = delete;

		[[nodiscard]] u32 max_entities() const noexcept { return m_max_entities; }
		[[nodiscard]] u32 component_count() const noexcept { return static_cast<u32>(m_components.size()); }
		[[nodiscard]] u32 prefab_count() const noexcept { return static_cast<u32>(m_prefabs.size()); }

		[[nodiscard]] const ecs::ComponentInfo& component(ComponentId id) const noexcept { return *m_components[id]; }
		[[nodiscard]] const PrefabInfo& prefab(PrefabId id) const noexcept { return m_prefabs[id]; }

		/** A prefab's value for one of its components, as the bits it writes. */
		[[nodiscard]] const ComponentBits& prefab_wire(PrefabId id, ComponentId component) const noexcept;

		/** A prefab's values again, after World::retune_prefab(): the same components, new bits. */
		void retune(PrefabId id, const ecs::Prefab& prefab) noexcept;

		/** Whether entities made from a prefab replicate: whether it has a Replicated component. */
		[[nodiscard]] bool replicated(PrefabId id) const noexcept { return m_prefabs[id].components != 0; }

		/** The component types only an entity's owner receives, those clients draw, those owners predict. */
		[[nodiscard]] ComponentMask owner_only() const noexcept { return m_owner_only; }
		[[nodiscard]] ComponentMask interpolated() const noexcept { return m_interpolated; }
		[[nodiscard]] ComponentMask predicted() const noexcept { return m_predicted; }

		/** Bits on the wire of an entity index, a prefab id and a component id. */
		[[nodiscard]] u32 index_bits() const noexcept { return m_index_bits; }
		[[nodiscard]] u32 prefab_bits() const noexcept { return m_prefab_bits; }
		[[nodiscard]] u32 component_bits() const noexcept { return m_component_bits; }

	private:
		u32 m_max_entities			 = 0;
		u32 m_index_bits			 = 0;
		u32 m_prefab_bits			 = 0;
		u32 m_component_bits		 = 0;
		ComponentMask m_owner_only	 = 0;
		ComponentMask m_interpolated = 0;
		ComponentMask m_predicted	 = 0;

		Vector<const ecs::ComponentInfo*> m_components; // by net id
		Vector<PrefabInfo> m_prefabs;
		Vector<ComponentBits> m_wires; // every prefab's values, a run per prefab
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
