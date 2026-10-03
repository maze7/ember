#pragma once

#include <ember/net/replication.h>

namespace ember::net
{
	/** Timed samples a replica keeps of an entity's Interpolated and Predicted components. */
	inline constexpr u32 REPLICA_SAMPLES = 8;

	struct ReplicaDef
	{
		u32 max_entities = 4096; // the server's ReplicatorDef::max_entities
	};

	/**
	 * The client's half of replication: the server's entities in the client's world, as the server's
	 * packets describe them.
	 *
	 * Every record leaves its entity exactly as the server had it at the packet's tick. An entity the
	 * server creates appears as this client makes its prefab (its Sim and Client components) with a
	 * NetId, and Owned and Simulated when this client controls it; records bring what differs from the
	 * prefab: values, and Replicated components gained or lost along the way. A removal destroys it.
	 *
	 * Values land in the world's components as they arrive, but for two kinds the replica keeps
	 * timed samples instead: one at the tick each state began, and one just before it when the state
	 * before held until then, so a stop and a start are drawn when they happened rather than smeared
	 * across the gap between updates.
	 *
	 *   Interpolated   interpolate() draws them at a moment a little in the past, every frame
	 *   Predicted      on an entity this client owns, its own simulation writes them; server_value()
	 *                  is the server's word, at tick(), to check the prediction against
	 *
	 * A section is applied whole or not at all. One that does not decode changes nothing, and the
	 * packet that carried it goes unacknowledged, so to the server it was lost.
	 *
	 * The client's own code leaves replicated entities to the replica: it neither destroys them nor
	 * takes their Replicated components away. React to them with systems, or with EnTT's signals.
	 * Single threaded: the client feeds it on the game thread, between the world's runs.
	 */
	class Replica final
	{
	public:
		/** world is a client's, complete, and outlives the replica. */
		explicit Replica(ecs::World& world, const ReplicaDef& def = {}) noexcept;

		Replica(const Replica&)			   = delete;
		Replica& operator=(const Replica&) = delete;

		/** Destroys every replicated entity: a new session starts here. The client calls this. */
		void reset() noexcept;

		/**
		 * Reads one packet's entity section and applies it to the world; false, having changed nothing,
		 * when it does not decode. The client calls this.
		 */
		[[nodiscard]] bool read(serialize::ReadStream& stream) noexcept;

		/**
		 * Writes every Interpolated component, but those this client predicts, as it stood at tick: a
		 * fractional server tick, a little behind the newest so there are samples either side. Before
		 * the oldest sample or after the newest, the nearest; nothing is extrapolated. Call it each frame
		 * before the Present systems.
		 */
		void interpolate(f64 tick) noexcept;

		/** The world entity a NetId names; NO_ENTITY for one this client does not have. */
		[[nodiscard]] ecs::Entity entity(NetId id) const noexcept;

		/** The server tick a replicated entity's state is known at; NO_TICK for one that is not. */
		[[nodiscard]] Tick tick(ecs::Entity entity) const noexcept;

		/**
		 * The server's newest value of an Interpolated or Predicted component, as of tick(entity): what
		 * a prediction is checked against. False when the entity is not replicated or has no such
		 * component.
		 */
		template <ecs::Component T> [[nodiscard]] bool server_value(ecs::Entity entity, T& out) const noexcept
		{
			static_assert(has_any(ecs::kind_of<T>, ecs::Kind::Interpolated | ecs::Kind::Predicted),
						  "the world has the newest value of the rest");
			const void* value = newest(entity, entt::type_hash<T>::value());
			if (value != nullptr)
				std::memcpy(&out, value, sizeof(T));
			return value != nullptr;
		}

		/** The same, for code that holds a ComponentInfo rather than the type: null when there is none. */
		[[nodiscard]] const void* server_value(ecs::Entity entity, const ecs::ComponentInfo& component) const noexcept
		{
			return newest(entity, component.type);
		}

		/** The newest server tick a section described; NO_TICK before the first. */
		[[nodiscard]] Tick latest_tick() const noexcept { return m_latest; }

		/** Replicated entities alive. */
		[[nodiscard]] u32 entity_count() const noexcept { return m_alive; }

	private:
		static constexpr u32 NONE = ~0u;

		/** One index of the server's entity table, as this client has it. */
		struct Slot
		{
			ecs::Entity entity		 = ecs::NO_ENTITY; // the world's
			Tick tick				 = NO_TICK;		   // the server tick its state is known at
			Tick since				 = NO_TICK;		   // the tick that state began
			ComponentMask components = 0;
			PrefabId prefab			 = 0;
			u16 generation			 = 0;
			u8 first				 = 0; // the oldest sample's place in the ring
			u8 samples				 = 0;
			bool alive				 = false;
			bool owned				 = false;

			std::array<Tick, REPLICA_SAMPLES> ticks = {}; // each sample's tick, a ring
		};

		/**
		 * One sampled component type, packed, for every entity that has one: the newest value, then a
		 * ring of samples in step with its entity's ring of ticks.
		 */
		struct Pool
		{
			Vector<u32> sparse; // by entity index: its place in dense, NONE when it has none
			Vector<u32> dense;	// the entity index of each
			Vector<u8> values;	// 1 + REPLICA_SAMPLES values apiece
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

		/** What of a prefab a client gets: all of it for the owner, less the OwnerOnly components for anyone else. */
		[[nodiscard]] ComponentMask seen_in_prefab(PrefabId prefab, bool owned) const noexcept;

		/** The components a slot keeps samples of: those drawn, and those its owner predicts. */
		[[nodiscard]] ComponentMask sampled(const Slot& slot) const noexcept;

		/** The components of a slot's entity the replica writes into the world as values arrive. */
		[[nodiscard]] ComponentMask written(const Slot& slot) const noexcept;

		[[nodiscard]] bool parse(serialize::ReadStream& stream, Tick& tick) noexcept;
		void apply(Tick tick) noexcept;
		void spawn(const Staged& staged) noexcept;
		void remove(u32 index) noexcept;

		/** Gives a slot samples of a component, every one of them this value. */
		void insert(u32 index, ComponentId component, const void* value) noexcept;
		void erase(u32 index, ComponentId component) noexcept;

		/** A sampled component's newest value (place NEWEST) or one of its samples. */
		[[nodiscard]] u8* value_of(u32 index, ComponentId component, u32 place) noexcept;
		[[nodiscard]] const u8* value_of(u32 index, ComponentId component, u32 place) const noexcept;
		[[nodiscard]] const void* newest(ecs::Entity entity, entt::id_type type) const noexcept;

		/** The ring place of a slot's i-th sample, oldest first. */
		[[nodiscard]] static u32 place_of(const Slot& slot, u32 i) noexcept;

		/** Opens a sample at tick, dropping the oldest when the ring is full; returns its ring place. */
		[[nodiscard]] static u32 push_sample(Slot& slot, Tick tick) noexcept;

		static constexpr u32 NEWEST = REPLICA_SAMPLES; // value_of's place for the newest value

		ecs::World& m_world;
		Schema m_schema;
		Vector<Pool> m_pools;	 // by component id; empty for one that is not sampled
		Vector<Slot> m_entities; // by index
		u32 m_alive	  = 0;
		Tick m_latest = NO_TICK;

		// A section on its way in: applied once all of it has decoded.
		Vector<u32> m_staged_removals;
		Vector<Staged> m_staged;
		Vector<u8> m_staged_values;
	};
}
