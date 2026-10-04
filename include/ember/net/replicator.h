#pragma once

#include <ember/net/connection.h>
#include <ember/net/replication.h>

namespace ember::net
{
	struct ReplicatorDef
	{
		u8 max_viewers	 = 16;					 // seats: the server's max_clients at least
		u32 max_bits	 = MAX_PACKET_BYTES * 8; // entity bits per packet at most, besides what the packet has left
		u32 max_entities = 4096;				 // replicated entities alive at once: the same on every machine
	};

	/** What the replicator put in a viewer's latest packet, and what it left for a later one. */
	struct SectionStats
	{
		u32 records	 = 0; // entities written
		u32 removals = 0;
		u32 waiting	 = 0; // records and removals owed that did not fit: they wait for a later packet
		u32 bits	 = 0; // the section's size
	};

	/**
	 * The server's half of replication: the server world's replicated entities as the wire sees them,
	 * and what each viewer (a seat) has of them.
	 *
	 * The game plays its world as it would alone, and the replicator follows it. An entity spawned
	 * from a prefab with a Replicated component replicates: it gets a NetId, its owner from the Owner
	 * component it spawned with, and it is destroyed with its world entity. Its Replicated components
	 * travel as they come, go and change, however they change: update() looks at every one each tick.
	 * A new entity starts as its prefab, which every client knows already, so only what differs from
	 * the prefab travels. Every component is kept as the bits its serialize() wrote, with the tick
	 * those bits last changed, so a value that moves less than the wire can show is no change at all.
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
	 * priority grows each packet it waits, by its Priority times the game's relevance for that viewer,
	 * so nothing starves. The entity a viewer owns goes into every one of its packets, even unchanged,
	 * so the client always has a server state at a known tick to check its prediction against.
	 *
	 * A Predicted component is snapped to wire precision in the world as it is written: the server goes
	 * on from the very value its clients decode, so an owner that snaps its prediction the same way
	 * (Prediction) can match the server to the bit.
	 *
	 * A destroyed entity's index is reused only once every viewer that had it has had the removal
	 * delivered, and then with the next generation: oldest free index first.
	 *
	 * Single threaded: the server's game thread drives it, between the world's runs. The server calls
	 * update() once a tick, then write() and on_notice() for each seat.
	 */
	class Replicator final
	{
	public:
		/** world is the server's, and outlives the replicator. What it has spawned already replicates too. */
		explicit Replicator(ecs::World& world, const ReplicatorDef& def = {}) noexcept;
		~Replicator() noexcept;

		Replicator(const Replicator&)			 = delete;
		Replicator& operator=(const Replicator&) = delete;

		/**
		 * The world as it stands at the end of tick: entities spawned and destroyed since, components
		 * gained and lost, values changed. The server calls this before it writes the tick's packets.
		 *
		 * Once a tick: a second call for the same tick does nothing. A game that sets relevance calls it
		 * itself after its tick, so what the tick spawned has its NetId, then sets relevance, and the
		 * server's own call finds nothing left to do.
		 */
		void update(Tick tick) noexcept;

		/** The world entity a NetId names; NO_ENTITY for a dead id. */
		[[nodiscard]] ecs::Entity entity(NetId id) const noexcept;

		/** A world entity's NetId: NO_NET_ID for one that does not replicate, or not yet. */
		[[nodiscard]] NetId id(ecs::Entity entity) const noexcept;

		/** Replicated entities alive. */
		[[nodiscard]] u32 entity_count() const noexcept { return m_alive; }

		/** Seats it keeps a viewer for: def.max_viewers. */
		[[nodiscard]] u32 viewer_count() const noexcept { return static_cast<u32>(m_viewers.size()); }

		/**
		 * How much a viewer cares about an entity: 0 takes it out of the viewer's world (the entity a
		 * viewer owns stays in it), more sends its changes sooner. 1 until set, for every new entity and
		 * every new viewer: set between update() and the server's writes, and a new entity never reaches
		 * a viewer it is not for.
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

		/** What write() last put in the viewer's packet, and what it left waiting. */
		[[nodiscard]] const SectionStats& section(u8 viewer) const noexcept { return m_viewers[viewer].section; }

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

		/** One index of the entity table. */
		struct Slot
		{
			ecs::Entity entity = ecs::NO_ENTITY; // the world's
			Change all;							 // every component: what its owner sees change
			Change shared;						 // the components everyone gets: what other viewers see change
			Tick set_all			 = NO_TICK;	 // the last change to which components it has, as its owner sees them
			Tick set_shared			 = NO_TICK;	 // the same, as every other viewer sees them
			ComponentMask components = 0;		 // what it has
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

		/**
		 * One component type's values, packed, for every entity that has one: the bits, and the bytes
		 * they were written from, so a value nobody touched is never written again.
		 */
		struct Pool
		{
			const ecs::ComponentInfo* info	= nullptr;
			const entt::sparse_set* storage = nullptr; // the world's
			Vector<u32> sparse;	   // by entity index: its value's place in dense, NONE when it has none
			Vector<u32> dense;	   // the entity index of each value
			Vector<Stored> values; // beside dense
			Vector<u8> bytes;	   // beside dense, info->size apiece
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
			Vector<Known> known;	   // by entity index
			SectionStats section = {}; // the latest write()

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

		[[nodiscard]] Stored* find(u32 index, ComponentId component) noexcept;
		[[nodiscard]] const Stored* find(u32 index, ComponentId component) const noexcept;

		/** Gives an entity's slot a component, written from these bytes: its stored bits are the caller's. */
		Stored& insert(u32 index, ComponentId component, const void* bytes) noexcept;
		void erase(u32 index, ComponentId component) noexcept;

		/** The bits a value writes; false when it writes more than MAX_COMPONENT_BITS. */
		[[nodiscard]] bool encode(ComponentId component, const void* value, ComponentBits& out) noexcept;

		void spawned(entt::registry& registry, ecs::Entity entity) noexcept;

		/** A world entity's slot, as its prefab starts it. */
		void create(ecs::Entity entity, Tick tick) noexcept;

		/**
		 * The slot made the world entity's: the same Replicated components, at the same values. every
		 * looks at every value, not only those whose bytes changed since: a new slot's first match.
		 */
		void match(u32 index, Tick tick, bool every = false) noexcept;

		/** A Predicted value becomes what its owner decodes from these bits: in the world, and in the slot. */
		void snap(u32 index, ComponentId component, const ComponentBits& wire) noexcept;

		/** Ends a slot's entity: viewers that have it are told. */
		void destroy(u32 index) noexcept;

		/** What a viewer gets of an entity: all of it for the owner, less the OwnerOnly components for anyone else. */
		[[nodiscard]] ComponentMask seen(const Slot& slot, bool owner) const noexcept;
		[[nodiscard]] ComponentMask seen_in_prefab(const Slot& slot, bool owner) const noexcept;

		[[nodiscard]] static const Change& change_for(const Slot& slot, bool owner) noexcept
		{
			return owner ? slot.all : slot.shared;
		}

		[[nodiscard]] static Tick set_change_for(const Slot& slot, bool owner) noexcept
		{
			return owner ? slot.set_all : slot.set_shared;
		}

		/** Whether a record for a viewer tells the entity's components: they may differ from what it has. */
		[[nodiscard]] bool tells_set(const Slot& slot, const Known& known, bool owner) const noexcept;

		[[nodiscard]] u32 record_bits(u8 viewer, u32 index, const Known& known, Tick tick) const noexcept;
		void write_record(serialize::WriteStream& stream, u8 viewer, u32 index, const Known& known, Tick tick) noexcept;
		void settle(Viewer& viewer, const SentPacket& packet, bool delivered) noexcept;
		void release(u32 index) noexcept;
		void free_index(u32 index) noexcept;

		ecs::World& m_world;
		ReplicatorDef m_def;
		Schema m_schema;

		Vector<Slot> m_entities;  // by index
		Vector<Pool> m_pools;	  // by component id
		Vector<Viewer> m_viewers; // by seat
		Vector<u32> m_free;		  // free indices, a ring: the oldest freed is reused first
		u32 m_free_head	 = 0;
		u32 m_free_count = 0;
		u32 m_high		 = 0; // one past the highest index ever used
		u32 m_alive		 = 0;
		Tick m_updated	 = NO_TICK; // the tick update() last looked at the world for

		Vector<ecs::Entity> m_spawned; // world entities spawned since the last update

		// Scratch for write() and update().
		Vector<Owed> m_owed;
		Vector<u32> m_removals;
		PacketBuffer m_scratch;
	};
}
