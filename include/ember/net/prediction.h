#pragma once

#include <ember/net/command_stream.h>
#include <ember/net/replica.h>

namespace ember::net
{
	/** Ticks of prediction kept for each entity a client owns: as many as the commands that replay them. */
	inline constexpr u32 PREDICTION_HISTORY = COMMAND_HISTORY;

	/**
	 * Client-side prediction following the Overwatch netcode model.
	 *
	 * The client simulates what it owns ahead of the server, on the commands it sends. After each tick,
	 * record() snaps every owned entity's Predicted components to wire precision, as the server's replicator
	 * snaps its own, and keeps them as the prediction for that tick. The server's state of an owned entity
	 * comes back in every packet, for an older tick, and check() compares it at wire precision with what was
	 * predicted for the same tick. A match costs nothing. A miss puts the server's state for that tick back
	 * into the world, and the client runs every tick since again on the commands it sent for them, which leaves
	 * the world where the prediction should have been.
	 *
	 * Snapped on both ends, a deterministic simulation predicts the server to the bit, so a miss is something
	 * the client could not know: a push from the server's rules, another player, a command that arrived late.
	 *
	 * For a replay to reproduce its ticks, everything an owned entity's simulation carries from one tick to
	 * the next is Predicted (OwnerOnly | Predicted when nobody else needs it), and the client's Simulate systems
	 * change nothing else: no resources, and no spawns while replaying.
	 *
	 * Single threaded: the client calls it on the game thread, between the world's runs.
	 */
	class Prediction final
	{
	public:
		/** World and Replica are the client's and outlive it. */
		Prediction(ecs::World& world, const Replica& replica) noexcept;

		Prediction(const Prediction&)			 = delete;
		Prediction& operator=(const Prediction&) = delete;

		/** After simulating tick: snaps every owned entity's Predicted components and keeps them as tick's. */
		void record(Tick tick) noexcept;

		/**
		 * Holds the newest server state of every owned entity to what was predicted for its tick. On a miss,
		 * the world gets every owned entity's state as of the oldest tick that missed, and that tick is returned:
		 * run every tick after it again, recording each. NO_TICK when every prediction held.
		 */
		[[nodiscard]] Tick check() noexcept;

		/** Misses so far, for a debug overlay. */
		[[nodiscard]] u32 corrections() const noexcept { return m_corrections; }

	private:
		/** One owned entity's predictions: which components each tick had, and their snapped values. */
		struct Track
		{
			ecs::Entity entity = ecs::NO_ENTITY;
			Tick checked	   = NO_TICK;						 // the newest server tick held against the prediction
			TickRing<ComponentMask, PREDICTION_HISTORY> present; // by tick: a bit per m_types entry
			Vector<u8> values;
		};

		/** A track for every owned entity, and none for those gone or given up. */
		void sync() noexcept;

		/** Snaps the entity's Predicted components and keeps them as tick's. */
		void keep(Track& track, Tick tick) noexcept;

		[[nodiscard]] bool matches(const Track& track, Tick tick) noexcept;

		/** The entity's state as of tick: the server's when it is the tick the server spoke of, else its own. */
		void restore(Track& track, Tick tick) noexcept;

		/** The bits a value writes, into one of the scratch buffers; their count. */
		[[nodiscard]] u32 wire_form(const ecs::ComponentInfo& info, const void* value, u32 scratch) noexcept;

		/** A value becomes what its own bits read back as: what the server makes of it. */
		void snap(const ecs::ComponentInfo& info, void* value) noexcept;

		[[nodiscard]] u8* row(Track& track, Tick tick) const noexcept;
		[[nodiscard]] const u8* row(const Track&, Tick tick) const noexcept;

		ecs::World& m_world;
		const Replica& m_replica;
		Vector<const ecs::ComponentInfo*> m_types; // the Predicted components
		Vector<u32> m_offsets;					   // each one's place in a row
		u32 m_stride	  = 0;
		u32 m_corrections = 0;
		Vector<Track> m_tracks;
		std::array<PacketBuffer, 2> m_scratch;
	};
}
