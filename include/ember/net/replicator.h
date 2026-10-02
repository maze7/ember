#pragma once

#include <ember/net/connection.h>
#include <ember/net/replication.h>

namespace ember::net
{
	/** An entity no client controls. */
	inline constexpr u8 NO_OWNER = 0xff;

	struct ReplicatorDef
	{
		u8 max_viewers = 16;				   // seats: the server's max_clients at least
		u32 max_bits   = MAX_PACKET_BYTES * 8; // entity bits per packet at most, besides what the packet has left
	};

	/**
	 * The server's half of replication: the replicated state of every entity, and what each viewer
	 * (a seat) has of it.
	 *
	 * The game creates entities from prefabs, gives them components, sets them and takes them away as
	 * it simulates, and destroys them. A new entity has its prefab's components at the prefab's values,
	 * which every client knows already, so only what differs from the prefab travels. Every component is
	 * kept as the bits its serialize() wrote, with the tick those bits last changed, so a value that
	 * moves less than the wire can show is no change at all: the game may set what it wrote this tick
	 * without checking whether it really changed.
	 *
	 * For each viewer and entity the replicator keeps the newest tick of that entity the viewer is
	 * known to have, learned from the connection's delivery notices. A record carries every component
	 * changed since then, and the entity's component set if that changed since then, so applying any
	 * record leaves the viewer's copy exactly the server's entity at the packet's tick: never half
	 * updated, with no baselines kept on either end. A record is sent once and sent again only when its
	 * packet is lost or the entity changes, and creates and removals are repeated until they are known
	 * to have arrived.
	 *
	 * Each packet takes removals first, then records by priority until its bit budget: an entity's
	 * priority grows each packet it waits, by its prefab's priority times the game's relevance for
	 * that viewer, so nothing starves. The entity a viewer owns goes into every one of its packets,
	 * even unchanged, so the client always has a server state at a known tick to check its
	 * prediction against.
	 *
	 * A destroyed entity's index is reused only once every viewer that had it has had the removal
	 * delivered, and then with the next generation: oldest free index first.
	 *
	 * Single threaded: the server's game thread drives it, and the server calls write() and
	 * on_notice() for each seat.
	 */
	class Replicator final
	{
	public:
		Replicator(const Schema& schema, const ReplicatorDef& def = {}) noexcept;

		Replicator(const Replicator&)			 = delete;
		Replicator& operator=(const Replicator&) = delete;

		/**
		 * A new entity during tick, with its prefab's components at the prefab's values. owner is the seat
		 * that controls it, which alone receives its OwnerOnly components, for as long as that seat's
		 * session lasts. key is the game's own tag, sent only to the owner, which matches an entity the
		 * client predicted (a shot it fired) with the server's. NO_NET_ID when every index is taken.
		 */
		[[nodiscard]] NetId create(PrefabId prefab, Tick tick, u8 owner = NO_OWNER, u32 key = 0) noexcept;

		/** Ends an entity. Viewers that have it are told; ids for it are dead from here on. */
		void destroy(NetId id) noexcept;

		/** Gives an entity a component during tick, at this value: a status effect, a shield. Sets it if it has one. */
		void add(NetId id, ComponentId component, const void* value, Tick tick) noexcept;

		/** Sets a component the entity has, during tick. Only a change on the wire counts as a change. */
		void set(NetId id, ComponentId component, const void* value, Tick tick) noexcept;

		/** Takes a component away from an entity during tick. */
		void remove(NetId id, ComponentId component, Tick tick) noexcept;

		template <class T> void add(NetId id, const T& value, Tick tick) noexcept { add(id, id_of<T>(), &value, tick); }
		template <class T> void set(NetId id, const T& value, Tick tick) noexcept { set(id, id_of<T>(), &value, tick); }
		template <class T> void remove(NetId id, Tick tick) noexcept { remove(id, id_of<T>(), tick); }

		/**
		 * A component as clients will decode it: the value at the wire's precision. A server that snaps
		 * its own state to this each tick simulates exactly what its clients predict. False when the
		 * entity is gone or has no such component.
		 */
		[[nodiscard]] bool get(NetId id, ComponentId component, void* out) const noexcept;

		template <class T> [[nodiscard]] bool get(NetId id, T& out) const noexcept { return get(id, id_of<T>(), &out); }

		/** The components a living entity has; none for a dead id. */
		[[nodiscard]] ComponentMask components(NetId id) const noexcept;

		[[nodiscard]] bool alive(NetId id) const noexcept;

		/** Living entities. */
		[[nodiscard]] u32 entity_count() const noexcept { return m_alive; }

		/** Seats it keeps a viewer for: def.max_viewers. */
		[[nodiscard]] u32 viewer_count() const noexcept { return static_cast<u32>(m_viewers.size()); }

		/**
		 * How much a viewer cares about an entity: 0 takes it out of the viewer's world (the entity a
		 * viewer owns stays in it), more sends its changes sooner. 1 until set, for every new entity and
		 * every new viewer.
		 */
		void set_relevance(u8 viewer, NetId id, f32 relevance) noexcept;

		/** A seat's session starts: its viewer has nothing yet. The server calls this. */
		void add_viewer(u8 viewer) noexcept;

		/**
		 * A seat's session ends: everything it had is let go, and its entities lose their owner. The
		 * server calls this.
		 */
		void remove_viewer(u8 viewer) noexcept;

		/**
		 * Writes a viewer's entity section into the packet with this sequence: the world as it stands
		 * at tick, every change for it made. Takes what is left of the packet, def.max_bits at most. The
		 * stream is a packet_writer() stream. The server calls this.
		 */
		void write(u8 viewer, serialize::WriteStream& stream, Sequence sequence, Tick tick) noexcept;

		/** What became of a packet write() filled for the viewer. The server calls this. */
		void on_notice(u8 viewer, const PacketNotice& notice) noexcept;

	private:
		static constexpr u32 NONE = ~0u;

		enum class Presence : u8
		{
			Absent,	  // the viewer does not have it
			Present,  // the viewer has it, or a create is on its way
			Removing, // a removal is owed or on its way
		};

		/** When a state began on the wire, and when the one before it did. */
		struct Change
		{
			Tick latest	  = NO_TICK;
			Tick previous = NO_TICK;

			void at(Tick tick) noexcept
			{
				if (tick != latest)
				{
					previous = latest;
					latest	 = tick;
				}
			}
		};

		struct Entity
		{
			Change all;							// every component: what its owner sees change
			Change shared;						// the components everyone gets: what other viewers see change
			Tick set_all			 = NO_TICK; // the last change to which components it has, as its owner sees them
			Tick set_shared			 = NO_TICK; // the same, as every other viewer sees them
			ComponentMask components = 0;		// what it has
			u32 key					 = 0;
			PrefabId prefab			 = 0;
			u16 generation			 = 1; // of the entity in the index, or of the next one
			u8 owner				 = NO_OWNER;
			u8 viewers				 = 0; // viewers whose presence is not Absent: the index is held while any is
			bool alive				 = false;
			bool used				 = false; // alive, or dead with viewers still to tell
		};

		struct Stored
		{
			Tick changed = NO_TICK; // NO_TICK: still the prefab's value
			ComponentBits wire;
		};

		/** One component type's values, packed, for every entity that has one. */
		struct Pool
		{
			Vector<u32> sparse;	   // by entity index: its value's place in dense, NONE when it has none
			Vector<u32> dense;	   // the entity index of each value
			Vector<Stored> values; // beside dense
		};

		/** One viewer's side of one entity. */
		struct Known
		{
			Tick acked		  = NO_TICK; // the newest tick of the entity the viewer is known to have
			Tick sent_tick	  = NO_TICK; // the tick of the newest record or removal on its way
			Sequence sent	  = 0;		 // that packet
			bool in_flight	  = false;	 // a record or removal is on its way, so nothing new is owed
			Presence presence = Presence::Absent;
			f32 relevance	  = 1.0f;
			f32 priority	  = 0.0f; // grows each packet the entity waits to be sent
		};

		/** One record or removal a packet carried, for its notice. */
		struct Sent
		{
			u32 index	   = 0;
			u16 generation = 0;
			bool removal   = false;
		};

		struct SentPacket
		{
			Sequence sequence = 0;
			Tick tick		  = NO_TICK;
			u32 count		  = 0; // its Sent entries, next in the queue
		};

		struct Viewer
		{
			bool active = false;
			Vector<Known> known; // by entity index

			// Packets awaiting their notices, oldest first, and their entries; each queue is read from
			// its head and compacted now and then.
			Vector<SentPacket> packets;
			Vector<Sent> entries;
			u32 packets_head = 0;
			u32 entries_head = 0;
		};

		struct Owed
		{
			f32 priority = 0.0f;
			f32 rate	 = 0.0f; // what the priority grows by each packet: breaks ties for the more urgent
			u32 index	 = 0;
		};

		template <class T> [[nodiscard]] ComponentId id_of() const noexcept
		{
			const ComponentId id = m_schema.id_of<T>();
			EMBER_ASSERT(id != NO_COMPONENT && "the schema has no such component");
			return id;
		}

		[[nodiscard]] Stored* find(u32 index, ComponentId component) noexcept;
		[[nodiscard]] const Stored* find(u32 index, ComponentId component) const noexcept;
		Stored& insert(u32 index, ComponentId component) noexcept;
		void erase(u32 index, ComponentId component) noexcept;

		/** The bits a value writes; false when it writes more than MAX_COMPONENT_BITS. */
		[[nodiscard]] bool encode(ComponentId component, const void* value, ComponentBits& out) noexcept;

		/** What a viewer gets of an entity: all of it for the owner, less the OwnerOnly components for anyone else. */
		[[nodiscard]] ComponentMask seen(const Entity& entity, bool owner) const noexcept;
		[[nodiscard]] ComponentMask seen_in_prefab(const Entity& entity, bool owner) const noexcept;

		[[nodiscard]] static const Change& change_for(const Entity& entity, bool owner) noexcept
		{
			return owner ? entity.all : entity.shared;
		}

		[[nodiscard]] static Tick set_change_for(const Entity& entity, bool owner) noexcept
		{
			return owner ? entity.set_all : entity.set_shared;
		}

		/** Whether a record for a viewer tells the entity's components: they may differ from what it has. */
		[[nodiscard]] bool tells_set(const Entity& entity, const Known& known, bool owner) const noexcept;

		[[nodiscard]] u32 record_bits(u8 viewer, u32 index, const Known& known, Tick tick) const noexcept;
		void write_record(serialize::WriteStream& stream, u8 viewer, u32 index, const Known& known, Tick tick) noexcept;
		void settle(Viewer& viewer, const SentPacket& packet, bool delivered) noexcept;
		void release(u32 index) noexcept;
		void free_index(u32 index) noexcept;

		const Schema& m_schema;
		ReplicatorDef m_def;

		Vector<Entity> m_entities; // by index
		Vector<Pool> m_pools;	   // by component id
		Vector<Viewer> m_viewers;  // by seat
		Vector<u32> m_free;		   // free indices, a ring: the oldest freed is reused first
		u32 m_free_head	 = 0;
		u32 m_free_count = 0;
		u32 m_high		 = 0; // one past the highest index ever used
		u32 m_alive		 = 0;

		// Scratch for write() and the setters.
		Vector<Owed> m_owed;
		Vector<u32> m_removals;
		PacketBuffer m_scratch;
	};
}
