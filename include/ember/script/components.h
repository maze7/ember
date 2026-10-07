#pragma once

#include <ember/core/common.h>
#include <ember/ecs/component.h>
#include <ember/net/serialize.h>

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

	/**
	 * Which stategraph drives an entity, which of its states it is in and since what tick: what a
	 * script's `stategraph "x" { ... }` runs over, replicated so a client can show the state and a save
	 * keeps it. A prefab gives it as `Stategraph = "x"`; the first tick enters the graph's initial state.
	 */
	struct Stategraph
	{
		static constexpr u8 NO_STATE = 0xff;

		GraphId graph;
		u8 state  = NO_STATE; // the state's index among the graph's states, sorted by name
		u32 since = 0;		  // the tick the state was entered

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, graph.value, 32);
			serialize_bits(stream, state, 8);
			serialize_bits(stream, since, 32);
			return true;
		}
	};

	EMBER_COMPONENT(Stategraph, Replicated);

	/**
	 * The engine components scripts drive: Stategraph, and anim::Playing for e:play(). A game registers
	 * them with its own, before its prefabs: registry order is how the wire names them.
	 */
	void register_components(ecs::Registry& registry) noexcept;
}
