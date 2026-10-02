#pragma once

#include <ember/net/replication.h>

namespace ember::net
{
	/** Timed samples a replica keeps of an entity's interpolated components. */
	inline constexpr u32 REPLICA_SAMPLES = 8;

	enum class ReplicaEventKind : u8
	{
		Created, // an entity entered this client's world, as its prefab makes it
		Updated, // it gained or lost components, or some got new values
		Removed, // it left: destroyed on the server, or out of this client's relevance
		Count
	};

	struct ReplicaEvent
	{
		ReplicaEventKind kind = ReplicaEventKind::Created;
		NetId id			  = NO_NET_ID;
		PrefabId prefab		  = 0;
		bool owned			  = false;	 // this client controls it, and predicts it
		u32 key				  = 0;		 // Created: the game's tag, on an entity this client owns
		Tick tick			  = NO_TICK; // the server tick of the packet that brought the news
		ComponentMask added	  = 0;		 // Updated: components it gained
		ComponentMask removed = 0;		 // Updated: components it lost
		ComponentMask written = 0;		 // Updated: components with new values, gained ones among them
	};

	/**
	 * The client's half of replication: its copy of the server's entities, as the server's packets
	 * describe them.
	 *
	 * Every record leaves its entity exactly as the server had it at the packet's tick. A new entity
	 * starts as its prefab makes it, and records bring what differs: values, and components gained or
	 * lost along the way. The replica keeps each component's newest value and, for interpolated
	 * components, a few timed samples to draw between: one at the tick each state began, and one just
	 * before it when the state before held until then, so a stop and a start are drawn when they
	 * happened rather than smeared across the gap between updates.
	 *
	 * A section is applied whole or not at all. One that does not decode changes nothing, and the
	 * packet that carried it goes unacknowledged, so to the server it was lost.
	 *
	 * What happens comes out as events from poll(): Created with the prefab, then Updated for whatever
	 * differs from it, Updated as entities change, and Removed. The game mirrors them in its own world
	 * and reads values with get() and sample(). The schema must be complete before the replica is made,
	 * and outlive it. Single threaded: the client feeds it on the game thread.
	 */
	class Replica final
	{
	public:
		explicit Replica(const Schema& schema) noexcept;

		Replica(const Replica&)			   = delete;
		Replica& operator=(const Replica&) = delete;

		/** Forgets every entity, with no events: a new session starts here. The client calls this. */
		void reset() noexcept;

		/**
		 * Reads one packet's entity section and applies it; false, having changed nothing, when it does
		 * not decode. The client calls this.
		 */
		[[nodiscard]] bool read(serialize::ReadStream& stream) noexcept;

		/** The next thing that happened, oldest first; false once there is none. */
		[[nodiscard]] bool poll(ReplicaEvent& event) noexcept;

		[[nodiscard]] bool alive(NetId id) const noexcept;

		/** What an entity was made from, and whether this client owns it. Only for living ids. */
		[[nodiscard]] PrefabId prefab(NetId id) const noexcept;
		[[nodiscard]] bool owned(NetId id) const noexcept;

		/** The components an entity has now; none for a dead id. */
		[[nodiscard]] ComponentMask components(NetId id) const noexcept;

		/** The server tick the entity's state is known at: for its owner, what a prediction is checked against. */
		[[nodiscard]] Tick tick(NetId id) const noexcept;

		/** A component's newest value. False when the entity is gone or has no such component. */
		[[nodiscard]] bool get(NetId id, ComponentId component, void* out) const noexcept;

		template <class T> [[nodiscard]] bool get(NetId id, T& out) const noexcept { return get(id, id_of<T>(), &out); }

		/**
		 * An interpolated component around tick, a fractional server tick: the samples either side, and
		 * how far from the first to the second tick lies. Before the oldest sample or after the newest,
		 * both are that sample and t is 0: nothing is extrapolated. False when the entity is gone or has
		 * no such component.
		 */
		[[nodiscard]] bool sample(NetId id, ComponentId component, f64 tick, void* from, void* to,
								  f32& t) const noexcept;

		template <class T> [[nodiscard]] bool sample(NetId id, f64 tick, T& from, T& to, f32& t) const noexcept
		{
			return sample(id, id_of<T>(), tick, &from, &to, t);
		}

		/** The newest server tick a section described; NO_TICK before the first. */
		[[nodiscard]] Tick latest_tick() const noexcept { return m_latest; }

		/** Living entities. */
		[[nodiscard]] u32 entity_count() const noexcept { return m_alive; }

	private:
		static constexpr u32 NONE = ~0u;

		struct Entity
		{
			Tick tick				 = NO_TICK; // the server tick its state is known at
			Tick since				 = NO_TICK; // the tick that state began
			ComponentMask components = 0;
			u32 key					 = 0;
			PrefabId prefab			 = 0;
			u16 generation			 = 0;
			u8 first				 = 0; // the oldest sample's place in the ring
			u8 samples				 = 0;
			bool alive				 = false;
			bool owned				 = false;

			std::array<Tick, REPLICA_SAMPLES> ticks = {}; // each sample's tick, a ring
		};

		/**
		 * One component type's values, packed, for every entity that has one. An interpolated type keeps
		 * a ring of samples per value too, in step with its entity's ring of ticks.
		 */
		struct Pool
		{
			Vector<u32> sparse; // by entity index: its value's place in dense, NONE when it has none
			Vector<u32> dense;	// the entity index of each value
			Vector<u8> values;	// the newest of each, a component's size apiece
			Vector<u8> samples; // REPLICA_SAMPLES values apiece, for an interpolated type
		};

		/** One record, decoded but not yet applied. */
		struct Staged
		{
			Tick changed			 = NO_TICK;
			Tick previous			 = NO_TICK;
			ComponentMask components = 0; // what the entity has once the record applies
			ComponentMask carried	 = 0; // the components whose values follow, from `values` in id order
			u32 index				 = 0;
			u32 key					 = 0;
			u32 values				 = 0; // where its values start in m_staged_values
			PrefabId prefab			 = 0;
			u16 generation			 = 0;
			bool create				 = false;
			bool owned				 = false;
		};

		template <class T> [[nodiscard]] ComponentId id_of() const noexcept
		{
			const ComponentId id = m_schema.id_of<T>();
			EMBER_ASSERT(id != NO_COMPONENT && "the schema has no such component");
			return id;
		}

		/** What of a prefab a client gets: all of it for the owner, less the OwnerOnly components for anyone else. */
		[[nodiscard]] ComponentMask seen_in_prefab(PrefabId prefab, bool owned) const noexcept;

		[[nodiscard]] bool parse(serialize::ReadStream& stream, Tick& tick) noexcept;
		void apply(Tick tick) noexcept;
		void spawn(const Staged& staged, Tick tick) noexcept;
		void remove(u32 index, Tick tick) noexcept;

		/** Gives an entity a component at a value, every sample of it that value too. */
		void insert(u32 index, ComponentId component, const void* value) noexcept;
		void erase(u32 index, ComponentId component) noexcept;
		[[nodiscard]] u8* value_of(u32 index, ComponentId component) noexcept;
		[[nodiscard]] const u8* value_of(u32 index, ComponentId component) const noexcept;
		[[nodiscard]] u8* sample_of(u32 index, ComponentId component, u32 place) noexcept;
		[[nodiscard]] const u8* sample_of(u32 index, ComponentId component, u32 place) const noexcept;

		/** The ring place of an entity's i-th sample, oldest first. */
		[[nodiscard]] static u32 place_of(const Entity& entity, u32 i) noexcept;

		/** Opens a sample at tick, dropping the oldest when the ring is full; returns its ring place. */
		[[nodiscard]] static u32 push_sample(Entity& entity, Tick tick) noexcept;

		const Schema& m_schema;
		ComponentMask m_interpolated = 0; // the interpolated component types
		Vector<Pool> m_pools;			  // by component id
		Vector<Entity> m_entities;		  // by index
		u32 m_alive	  = 0;
		Tick m_latest = NO_TICK;

		Vector<ReplicaEvent> m_events;
		u32 m_events_head = 0;

		// A section on its way in: applied once all of it has decoded.
		Vector<u32> m_staged_removals;
		Vector<Staged> m_staged;
		Vector<u8> m_staged_values;
	};
}

namespace ember
{
	EMBER_ENUM_NAMES(net::ReplicaEventKind, "Created", "Updated", "Removed");
}
