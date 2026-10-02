#include <ember/ecs/prefab.h>

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
}
