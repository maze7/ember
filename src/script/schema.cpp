#include "internal.h"

#include <ember/core/logger.h>
#include <ember/ecs/system.h>

#include <algorithm>
#include <cstring>

/**
 * The schema pass and what it leaves: the scripts' components into the registry as types, their
 * prefabs as prefabs, before any world is made. Every machine runs it over the same scripts and
 * registers the same things in the same order.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		void problem(Schema& schema, const PrefabDecl& decl, String message)
		{
			Problem made;
			made.path	 = decl.path;
			made.message = std::move(message);
			schema.problems.push_back(std::move(made));
		}

		/** Registers one declaration, its base first. False when it cannot be. */
		bool register_prefab(ecs::Registry& registry, Schema& schema, Vector<PrefabDecl>& decls, size_t index,
							 Vector<u8>& state)
		{
			enum : u8
			{
				Untried,
				Trying,
				Done,
				Failed
			};
			if (state[index] == Done)
				return true;
			if (state[index] == Failed)
				return false;
			if (state[index] == Trying)
			{
				problem(schema, decls[index], "prefab " + decls[index].name + " extends itself, through what it extends");
				state[index] = Failed;
				return false;
			}
			state[index] = Trying;

			PrefabDecl& decl		= decls[index];
			const ecs::Prefab* base = nullptr;

			if (!decl.extends.empty())
			{
				base = registry.prefab_types().find(StringView(decl.extends));
				if (base == nullptr)
				{
					// Another script prefab, registered first.
					size_t other = decls.size();
					for (size_t i = 0; i < decls.size(); ++i)
						if (decls[i].name == decl.extends)
							other = i;
					if (other == decls.size() || !register_prefab(registry, schema, decls, other, state))
					{
						problem(schema, decl, "prefab " + decl.name + " extends '" + decl.extends + "', which is not a prefab");
						state[index] = Failed;
						return false;
					}
					base = registry.prefab_types().find(StringView(decl.extends));
				}
			}

			ecs::Prefab prefab;
			String why(&heap());
			if (!compose_prefab(registry.component_types(), base, decl, true, prefab.components, why))
			{
				problem(schema, decl, "prefab " + decl.name + ": " + why);
				state[index] = Failed;
				return false;
			}
			prefab.name = String(decl.name, &memory::heap(MemoryTag::ECS));
			(void)registry.add_prefab(std::move(prefab));
			state[index] = Done;
			return true;
		}
	}

	bool compose_prefab(const ecs::Components& types, const ecs::Prefab* base, const PrefabDecl& decl, bool with_start_groups,
						Vector<ecs::PrefabComponent>& out, String& why) noexcept
	{
		out.clear();
		if (base != nullptr)
			for (const ecs::PrefabComponent& component : base->components)
				out.push_back(component);

		const auto find = [&](ecs::ComponentId id) -> ecs::PrefabComponent*
		{
			for (ecs::PrefabComponent& component : out)
				if (component.id == id)
					return &component;
			return nullptr;
		};

		// Each entry over what is there: the fields it gives, over the base's value or the type's defaults.
		const auto apply = [&](const Vector<PrefabEntry>& entries)
		{
			for (const PrefabEntry& entry : entries)
			{
				const ecs::ComponentInfo* info = types.find(StringView(entry.component));
				if (info == nullptr)
				{
					why = "no component named " + entry.component;
					return false;
				}
				if (entry.removed)
				{
					std::erase_if(out, [&](const ecs::PrefabComponent& component) { return component.id == info->id; });
					continue;
				}
				if (entry.bytes.size() != info->size || entry.mask.size() != info->size)
				{
					why = entry.component + " was converted for another layout: restart";
					return false;
				}

				ecs::PrefabComponent* held = find(info->id);
				if (held == nullptr)
				{
					ecs::PrefabComponent made;
					made.id	   = info->id;
					made.value = Vector<u8>(entry.bytes.begin(), entry.bytes.end(), &memory::heap(MemoryTag::ECS));
					out.push_back(std::move(made));
					continue;
				}
				for (u32 i = 0; i < info->size; ++i)
					if (entry.mask[i] != 0)
						held->value[i] = entry.bytes[i];
			}
			return true;
		};

		if (!apply(decl.entries))
			return false;
		if (with_start_groups)
			for (const String& start : decl.start)
				for (const PrefabGroup& group : decl.groups)
					if (group.name == start && !apply(group.entries))
						return false;

		std::sort(out.begin(), out.end(),
				  [](const ecs::PrefabComponent& a, const ecs::PrefabComponent& b) { return a.id < b.id; });
		return true;
	}

	void Schema::apply(ecs::Registry& registry) noexcept
	{
		// Components first, in name order, so every machine numbers them alike.
		for (const ecs::DynamicComponentDef& def : components)
		{
			if (registry.add_component(def) == ecs::NO_COMPONENT)
			{
				Problem made;
				made.message = "component " + def.name + " could not be registered: the name is taken, or it is too wide";
				problems.push_back(std::move(made));
			}
		}

		// Then prefabs, in name order, each after what it extends.
		Vector<u8> state(prefabs.size(), u8{0}, &heap());
		for (size_t i = 0; i < prefabs.size(); ++i)
		{
			if (registry.prefab_types().find(StringView(prefabs[i].name)) != nullptr && state[i] == 0)
			{
				problem(*this, prefabs[i], "prefab " + prefabs[i].name + ": a prefab of that name exists already");
				state[i] = 3;
				continue;
			}
			(void)register_prefab(registry, *this, prefabs, i, state);
		}

		for (const Problem& made : problems)
			EMBER_WARN("schema {}: {}", StringView(made.path), StringView(made.message));
	}

	Schema collect_schema(const ecs::Registry& registry, Span<const Source> sources, void (*bind)(Binding&, void* user),
						  void* user) noexcept
	{
		Schema schema;
		{
			// A scratch world on the registry as it stands: the C++ types, for the binding the declarations convert with.
			ecs::World world(registry);
			HostDef def;
			def.schema = &schema;
			Host host(world, def);
			if (bind != nullptr)
				bind(host.binding(), user);
			host.reload(sources);
			host.finish_schema();
			for (const Problem& made : host.problems())
				schema.problems.push_back(made);
		}
		return schema;
	}

	void register_components(ecs::Registry& registry) noexcept { registry.components<Stategraph, anim::Playing>(); }
}
