#include <ember/ecs/components.h>

namespace ember::ecs
{
	const ComponentInfo* Components::find(std::string_view name) const noexcept
	{
		for (const ComponentInfo& info : m_infos)
		{
			if (info.name == name)
				return &info;
		}

		return nullptr;
	}

	const ComponentInfo* Components::find(entt::id_type type) const noexcept
	{
		for (const ComponentInfo& info : m_infos)
		{
			if (info.type == type)
				return &info;
		}

		return nullptr;
	}
}
