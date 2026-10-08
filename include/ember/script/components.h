#pragma once

#include <ember/core/common.h>
#include <ember/core/hash.h>
#include <ember/ecs/component.h>
#include <ember/net/serialize.h>
#include <ember/physics/components.h>

#include <algorithm>
#include <array>

namespace ember::ecs
{
	class Registry;
}

namespace ember::script
{
	/** A stategraph's number: the low half of hash_text() of its name. Opaque to scripts, which name graphs. */
	struct GraphId
	{
		u32 value = 0;

		constexpr bool operator==(const GraphId&) const noexcept = default;
	};

	/** A state's number: the low half of hash_text() of its name. 0 is no state. */
	using StateId = u32;

	inline constexpr StateId NO_STATE = 0;

	[[nodiscard]] constexpr StateId state_id(StringView name) noexcept { return static_cast<StateId>(hash_text(name)); }
	[[nodiscard]] constexpr GraphId graph_id(StringView name) noexcept { return {static_cast<u32>(hash_text(name))}; }

	/**
	 * Which stategraphs drive an entity, which of their states each is in and since what tick: what a
	 * script's `stategraph "x" { ... }` runs over. Two slots, so a player carries its weapon's graph beside
	 * another. The state is its name's hash, so a reload that adds or removes states leaves every entity
	 * in the state it was in. Predicted: the owner's client runs a sim graph on its own player ahead of
	 * the server and is corrected; on everything else it is replicated, so a client can show the state
	 * and a save keeps it. A prefab gives it as `Stategraph = "x"` or `Stategraph = { "sword", "look" }`;
	 * the first tick enters each graph's initial state.
	 */
	struct Stategraph
	{
		static constexpr u32 SLOTS = 2;

		struct Slot
		{
			GraphId graph;
			StateId state = NO_STATE;
			u32 since	  = 0; // the tick the state was entered
		};

		std::array<Slot, SLOTS> slots = {};

		template <class Stream> bool serialize(Stream& stream)
		{
			for (Slot& slot : slots)
			{
				serialize_bits(stream, slot.graph.value, 32);
				serialize_bits(stream, slot.state, 32);
				serialize_bits(stream, slot.since, 32);
			}
			return true;
		}
	};

	EMBER_COMPONENT(Stategraph, Predicted);

	/**
	 * A hit window a graph's state opened with e:strike{...}: until when the entity's hitbox hits what the strike
	 * said, and what it hit before, which it goes back to when the strike ends, as its state ends at the latest.
	 * Predicted, so a client that runs its own player's graph puts it back with the rest on a correction.
	 */
	struct Strike
	{
		u32 until	= 0; // the tick it ends at
		u32 since	= 0; // its state's
		u32 restore = 0; // the hitbox's layers before it
		u8 slot		= 0;
		bool once	= false; // off after its first hit
		bool active = false;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, until, 32);
			serialize_bits(stream, since, 32);
			serialize_bits(stream, restore, 32);
			serialize_bits(stream, slot, 1);
			serialize_bool(stream, once);
			serialize_bool(stream, active);
			return true;
		}
	};

	EMBER_COMPONENT(Strike, Predicted);

	/** One modifier on an entity: which, until when, how many stacks, and how strong. */
	struct Modifier
	{
		u32 name   = 0; // the low half of its name's hash
		u32 until  = 0; // the tick it ends at; 0 for one that lasts until it is cured
		u16 stacks = 0;
		u16 power  = 0; // a whole number its tick reads: damage a tick, a percent
	};

	/**
	 * What lasts on an entity for a while: e:inflict() puts a modifier here, e:cure() takes it off, and every
	 * machine that simulates the entity counts it down, so its owner predicts what a slow does to its own steps.
	 * Sorted by name, so stats combine alike everywhere. A prefab gives it with `Modifiers = {}`; e:inflict()
	 * adds it when it is missing.
	 */
	struct Modifiers
	{
		static constexpr u32 CAPACITY = 4;

		std::array<Modifier, CAPACITY> list = {};
		u8 count							= 0;

		template <class Stream> bool serialize(Stream& stream)
		{
			u32 held = count;
			serialize_bits(stream, held, 3);
			if (Stream::IsReading)
			{
				if (held > CAPACITY)
					return false;
				count = static_cast<u8>(held);
			}
			for (u32 i = 0; i < count; ++i)
			{
				Modifier& one = list[i];
				serialize_bits(stream, one.name, 32);
				serialize_bits(stream, one.until, 32);
				serialize_bits(stream, one.stacks, 16);
				serialize_bits(stream, one.power, 16);
			}
			return true;
		}
	};

	EMBER_COMPONENT(Modifiers, Predicted);

	/**
	 * A place that notices what comes and goes: every tick the server finds the hurtboxes on `layers` that its
	 * shape, about the entity, touches, and raises "entered" on it for each that was not there the tick before
	 * and "left" for each that has gone, the other entity as `by`.
	 */
	struct Trigger
	{
		physics::Shape shape   = physics::circle(8.0f);
		physics::Layers layers = {};
	};

	EMBER_COMPONENT(Trigger, Sim);

	/** A stat's number, for Host::stat(): the hash of its name, "move_speed". */
	[[nodiscard]] constexpr u64 stat_id(StringView name) noexcept { return hash_text(name); }

	/** A Stategraph for a code prefab: `script::stategraphs("sword")`, or two. */
	[[nodiscard]] constexpr Stategraph stategraphs(StringView first, StringView second = {}) noexcept
	{
		Stategraph graphs;
		graphs.slots[0].graph = graph_id(first);
		if (!second.empty())
			graphs.slots[1].graph = graph_id(second);
		return graphs;
	}

	/**
	 * A moment clients answer to: an event the server raised on an entity that some client-side `show` names.
	 * Its name's low hash, the tick it happened, who caused it by network id, and one number of the raiser's.
	 */
	struct Cue
	{
		u32 name  = 0;
		u32 tick  = 0;
		u32 by	  = 0;
		f32 value = 0.0f;
	};

	/**
	 * The last few cues raised on an entity, as the server replicates them: a ring, newest last. A client
	 * shows each once, at the moment it is drawn happening, and tells one it has shown from one it has not
	 * by its tick and name. A prefab with a Stategraph or a show gets it; `Cues = {}` gives it to any other.
	 */
	struct Cues
	{
		static constexpr u32 CAPACITY = 3;

		std::array<Cue, CAPACITY> ring = {};
		u8 next						   = 0; // where the next cue goes
		u8 count					   = 0; // held, up to CAPACITY

		void push(Cue cue) noexcept
		{
			ring[next] = cue;
			next	   = static_cast<u8>((next + 1) % CAPACITY);
			count	   = static_cast<u8>(std::min<u32>(count + 1, CAPACITY));
		}

		template <class Stream> bool serialize(Stream& stream)
		{
			// Oldest first, as many as are held: a reader lays them out from the start of its ring.
			u32 held = count;
			serialize_bits(stream, held, 2);
			if (Stream::IsReading)
			{
				count = static_cast<u8>(std::min<u32>(held, CAPACITY));
				next  = static_cast<u8>(count % CAPACITY);
			}
			for (u32 i = 0; i < count; ++i)
			{
				Cue& cue = ring[Stream::IsWriting ? (next + CAPACITY - count + i) % CAPACITY : i];
				serialize_bits(stream, cue.name, 32);
				serialize_bits(stream, cue.tick, 32);
				serialize_bits(stream, cue.by, 32);
				serialize_float(stream, cue.value);
			}
			return true;
		}
	};

	EMBER_COMPONENT(Cues, Replicated);

	/**
	 * What a client has shown of an entity so far: each slot's state as drawn and how far its marks have
	 * fired, and the cues shown. The host keeps it on every entity it shows; nothing else reads it.
	 */
	struct Shown
	{
		std::array<u32, Modifiers::CAPACITY> modifiers = {}; // the modifiers whose shows it has entered
		u8 modifier_count							   = 0;

		struct Slot
		{
			StateId state = NO_STATE;
			u32 since	  = 0;
			u8 marks	  = 0;	   // marks of the state fired so far
			bool late	  = false; // the state was first seen long after it began: its one-shots were skipped
		};

		std::array<Slot, Stategraph::SLOTS> slots = {};
		u32 seen_tick							  = 0;	// the newest cue tick shown
		std::array<u32, Cues::CAPACITY> seen	  = {}; // the names shown at that tick
		u8 seen_count							  = 0;
		bool fresh								  = true; // never shown before: its prefab show enters

		[[nodiscard]] bool has_seen(u32 tick, u32 name) const noexcept
		{
			if (tick < seen_tick)
				return true;
			if (tick > seen_tick)
				return false;
			for (u32 i = 0; i < seen_count; ++i)
				if (seen[i] == name)
					return true;
			return false;
		}

		void see(u32 tick, u32 name) noexcept
		{
			if (tick > seen_tick)
			{
				seen_tick  = tick;
				seen_count = 0;
			}
			if (tick == seen_tick && seen_count < Cues::CAPACITY)
				seen[seen_count++] = name;
		}
	};

	EMBER_COMPONENT(Shown, Client);

	/**
	 * The stories an entity runs by name hash, two at most, beside any its prefab gives it: what e:start()
	 * writes and e:stop() clears. On every machine, and never on the wire: a story's thread is the host's,
	 * and each end runs the stories its directory names.
	 */
	struct Story
	{
		static constexpr u32 SLOTS = 2;

		std::array<u32, SLOTS> slots = {};
	};

	EMBER_COMPONENT(Story, Sim);

	/** A Story for a code prefab: `script::stories("den")`. */
	[[nodiscard]] constexpr Story stories(StringView first, StringView second = {}) noexcept
	{
		Story story;
		story.slots[0] = static_cast<u32>(hash_text(first));
		if (!second.empty())
			story.slots[1] = static_cast<u32>(hash_text(second));
		return story;
	}

	/**
	 * The engine components scripts drive: Stategraph, anim::Playing for e:play(), Cues for what clients show,
	 * and Shown, a client's own bookkeeping. A game registers them with its own, before its prefabs: registry
	 * order is how the wire names them.
	 */
	void register_components(ecs::Registry& registry) noexcept;
}
