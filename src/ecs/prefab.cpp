#include <ember/core/hash.h>
#include <ember/ecs/prefab.h>
#include <ember/ecs/system.h>

namespace ember::ecs
{
	const PrefabComponent* Prefab::find(ComponentId component) const noexcept
	{
		for (const PrefabComponent& candidate : components)
		{
			if (candidate.id == component)
				return &candidate;
		}

		return nullptr;
	}

	PrefabId Prefabs::add(Prefab prefab) noexcept
	{
		EMBER_ASSERT(find(prefab.name) == nullptr && "two prefabs of one name");
		EMBER_ASSERT((prefab.definition == nullptr || find_definition(prefab.definition) == nullptr) &&
					 "a prefab registered twice");

		prefab.id = static_cast<PrefabId>(m_prefabs.size());
		if (prefab.definition != nullptr)
			m_by_definition.emplace(prefab.definition, prefab.id);

		m_prefabs.push_back(std::move(prefab));
		return m_prefabs.back().id;
	}

	const Prefab* Prefabs::find(StringView name) const noexcept
	{
		for (const Prefab& prefab : m_prefabs)
		{
			if (prefab.name == name)
				return &prefab;
		}

		return nullptr;
	}

	const Prefab* Prefabs::find_definition(const void* definition) const noexcept
	{
		const auto found = m_by_definition.find(definition);
		return found != m_by_definition.end() ? &m_prefabs[found->second] : nullptr;
	}

	u64 Registry::fingerprint() const noexcept
	{
		u64 hash = HASH_SEED;
		for (const ComponentInfo& info : m_components.all())
		{
			hash = hash_text(info.name, hash);
			hash = hash_value(info.kind, hash);
			hash = hash_value(info.size, hash);
			for (const FieldInfo& field : info.fields)
			{
				hash = hash_text(field.name, hash);
				hash = hash_value(field.type, hash);
				hash = hash_value(field.offset, hash);
			}
		}
		for (u32 id = 0; id < m_prefabs.count(); ++id)
		{
			const Prefab& prefab = m_prefabs[id];
			hash				 = hash_text(prefab.name, hash);
			for (const PrefabComponent& component : prefab.components)
			{
				hash = hash_value(component.id, hash);
				// Only what both ends hold: a Client or Server value never crosses the wire, and its bytes may
				// hold pointers that differ between builds.
				if (has_any(m_components[component.id].kind, Kind::Sim))
					hash = hash_bytes({component.value.data(), component.value.size()}, hash);
			}
		}
		return hash;
	}
}
