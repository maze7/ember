#include <ember/core/json.h>
#include <ember/ecs/prefab.h>

#include <fmt/format.h>

#include <iterator>

namespace ember::ecs
{
	namespace
	{
		/** Puts the reason a file was refused into error. */
		template <class... Args> bool refuse(String& error, fmt::format_string<Args...> format, Args&&... args)
		{
			error.clear();
			fmt::format_to(std::back_inserter(error), format, std::forward<Args>(args)...);
			return false;
		}
	}

	const PrefabComponent* Prefab::find(ComponentId component) const noexcept
	{
		for (const PrefabComponent& candidate : components)
		{
			if (candidate.id == component)
				return &candidate;
		}

		return nullptr;
	}

	bool Prefabs::load(StringView name, StringView text, String& error) noexcept
	{
		Json json;
		JsonError parse_error;
		if (!json.parse(text, memory::heap(MemoryTag::ECS), JsonRead::Relaxed, &parse_error))
			return refuse(error, "{}:{}: {}", name, parse_error.line, parse_error.message);

		if (find(name) != nullptr)
			return refuse(error, "{}: a prefab of that name is loaded already", name);

		Prefab prefab;
		prefab.name = String(name);
		prefab.id	= static_cast<PrefabId>(m_prefabs.size());

		const JsonValue root = json.root();
		if (!root.is_object())
			return refuse(error, "{}: a prefab is an object, with components and maybe a base", name);

		for (const auto [key, value] : root.members())
		{
			if (key != "base" && key != "components")
				return refuse(error, "{}: {} is not part of a prefab, which has components and maybe a base", name,
							  key);
		}

		// A variant starts as its base, and changes only what it names.
		if (const JsonValue base = root["base"])
		{
			StringView base_name;
			if (!base.read(base_name))
				return refuse(error, "{}: base is the name of a prefab", name);

			const Prefab* parent = find(base_name);
			if (parent == nullptr)
				return refuse(error, "{}: base \"{}\" is not loaded", name, base_name);

			prefab.components = parent->components;
		}

		for (const auto [component_name, fields] : root["components"].members())
		{
			const ComponentInfo* info = m_components.find(component_name);
			if (info == nullptr)
				return refuse(error, "{}: no component is called {}", name, component_name);

			if (!fields.is_object())
				return refuse(error, "{}: {} takes an object of fields, {{}} for its defaults", name, component_name);

			PrefabComponent* component = nullptr;
			for (PrefabComponent& existing : prefab.components)
			{
				if (existing.id == info->id)
					component = &existing;
			}

			if (component == nullptr)
			{
				component		 = &prefab.components.emplace_back();
				component->id	 = info->id;
				component->value = info->defaults;
			}

			for (const auto [field_name, value] : fields.members())
			{
				const FieldInfo* field = nullptr;
				for (const FieldInfo& candidate : info->fields)
				{
					if (candidate.name == field_name)
						field = &candidate;
				}

				if (field == nullptr)
					return refuse(error, "{}: {} has no field {}", name, info->name, field_name);

				if (!field->read(component->value.data(), value))
					return refuse(error, "{}: {}.{} does not take that value", name, info->name, field_name);
			}
		}

		m_prefabs.push_back(std::move(prefab));
		return true;
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
}
