#pragma once

#include "ecs/scene.h"
namespace Ember
{
	using SystemFn = std::function<void(Scene&, double)>;

	class SystemRegistry
	{
	public:
		void add(const std::string& name, SystemFn fn) {
			m_systems.push_back({ name, std::move(fn) });
		}

		void remove(const std::string& name) {
			m_systems.erase(
				std::remove_if(m_systems.begin(), m_systems.end(),
					[&](const SystemEntry& entry) {
						return entry.name == name;
					}),
					m_systems.end()
			);
		}

		void update(Scene& scene, double dt) {
			for (const auto& system : m_systems) {
				system.fn(scene, dt);
			}
		}

	private:
		struct SystemEntry {
			std::string name;
			SystemFn fn;
		};

		std::vector<SystemEntry> m_systems;
	};
}
