#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/ecs/prefab.h>
#include <ember/memory/memory.h>
#include <ember/script/problem.h>
#include <ember/script/source.h>

namespace ember::ecs
{
	class Registry;
}

namespace ember::script
{
	class Binding;

	/** One component of a script's prefab, or of one of its groups: its value as bytes, or its removal. */
	struct PrefabEntry
	{
		String component{&memory::heap(MemoryTag::Scripting)};
		bool removed = false; // `Comp = false`: the base had it, this prefab does not
		Vector<u8> bytes{&memory::heap(MemoryTag::Scripting)}; // the type's defaults with the fields given over them
		Vector<u8> mask{&memory::heap(MemoryTag::Scripting)};  // a byte per byte: 1 where a field was given, so a base's value shows through the rest
	};

	struct PrefabGroup
	{
		String name{&memory::heap(MemoryTag::Scripting)};
		Vector<PrefabEntry> entries{&memory::heap(MemoryTag::Scripting)};
	};

	struct PrefabEvent
	{
		String name{&memory::heap(MemoryTag::Scripting)};
		Vector<String> add{&memory::heap(MemoryTag::Scripting)};	// groups the event adds
		Vector<String> remove{&memory::heap(MemoryTag::Scripting)}; // groups it takes away
	};

	/** A prefab as a script declared it, its values converted; what Schema::apply() registers. */
	struct PrefabDecl
	{
		String name{&memory::heap(MemoryTag::Scripting)};
		String path{&memory::heap(MemoryTag::Scripting)};	 // the declaring script
		String extends{&memory::heap(MemoryTag::Scripting)}; // a C++ prefab's name, or another script prefab's
		Vector<PrefabEntry> entries{&memory::heap(MemoryTag::Scripting)};
		Vector<PrefabGroup> groups{&memory::heap(MemoryTag::Scripting)};
		Vector<String> start{&memory::heap(MemoryTag::Scripting)}; // groups an entity starts with
		Vector<PrefabEvent> events{&memory::heap(MemoryTag::Scripting)};
	};

	/**
	 * What the scripts declare of a game's content: the components and prefabs their `component` and
	 * `prefab` calls describe. Collected by a host in schema mode, before any world exists, and applied
	 * to the registry the worlds are then made from: a script's component is a type like any C++ one
	 * from there on, and its prefab spawns by name. Every machine collects the same schema from the
	 * same scripts; Registry::fingerprint() tells two apart.
	 */
	struct Schema
	{
		Vector<ecs::DynamicComponentDef> components{&memory::heap(MemoryTag::Scripting)}; // by name
		Vector<PrefabDecl> prefabs{&memory::heap(MemoryTag::Scripting)};				   // by name
		Vector<String> stategraphs{&memory::heap(MemoryTag::Scripting)};				   // names declared
		Vector<Problem> problems{&memory::heap(MemoryTag::Scripting)};

		/**
		 * Registers the components, then the prefabs, each over what it extends. What cannot be
		 * registered is reported in `problems` and left out. Once, before the first world.
		 */
		void apply(ecs::Registry& registry) noexcept;
	};

	/**
	 * The schema pass: a scratch world on the registry, a host in schema mode with the game's binding,
	 * every source loaded once for its declarations alone. Systems and stategraphs are not run or kept.
	 */
	[[nodiscard]] Schema collect_schema(const ecs::Registry& registry, Span<const Source> sources,
										void (*bind)(Binding&, void* user), void* user) noexcept;

	/** collect_schema() then apply(): what a game calls between registering its C++ content and making worlds. */
	template <class Bind>
	Schema apply_schema(ecs::Registry& registry, Span<const Source> sources, Bind&& bind) noexcept
	{
		Schema schema = collect_schema(
			registry, sources, [](Binding& binding, void* user) { (*static_cast<std::remove_reference_t<Bind>*>(user))(binding); },
			&bind);
		schema.apply(registry);
		return schema;
	}
}
