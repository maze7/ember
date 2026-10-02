#pragma once

#include <ember/ecs/components.h>

namespace ember::ecs
{
	/** A prefab's place in its Prefabs: the order they were loaded in, the same on every machine. */
	using PrefabId = u32;

	/** One component of a prefab: which, and its whole value, the type's defaults with the file's fields over them. */
	struct PrefabComponent
	{
		ComponentId id = 0;
		Vector<u8> value;
	};

	/**
	 * An entity as a designer wrote it: every component it has, wherever each one lives. A server
	 * makes its Sim and Server components, a client its Sim and Client ones.
	 */
	struct Prefab
	{
		String name;
		PrefabId id = 0;
		Vector<PrefabComponent> components;

		/** Its value for a component, or null when it has none. */
		[[nodiscard]] const PrefabComponent* find(ComponentId component) const noexcept;
	};

	/**
	 * Prefabs by name, read from prefab files:
	 *
	 *     {
	 *         "base": "enemies/base",   // optional: start from a prefab loaded before
	 *         "components": {
	 *             "HealthComponent": { "current": 30, "max": 30 },
	 *             "SpriteComponent": { "sheet": 2, "tint": [1, 0.8, 0.8, 1] },
	 *         },
	 *     }
	 *
	 * Comments and trailing commas are fine: people write these. A field takes a number, a bool, an
	 * enum value by name or a vector as an array; a field the file leaves out keeps the base's value,
	 * or the type's default.
	 */
	class Prefabs final
	{
	public:
		/** components must hold every type the files name, and outlive the prefabs. */
		explicit Prefabs(const Components& components) noexcept : m_components(components) {}

		Prefabs(const Prefabs&)			   = delete;
		Prefabs& operator=(const Prefabs&) = delete;

		/**
		 * Reads one prefab file. False with the reason in error, and nothing added: a designer's typo
		 * is a message, not a crash.
		 */
		[[nodiscard]] bool load(StringView name, StringView text, String& error) noexcept;

		[[nodiscard]] const Prefab* find(StringView name) const noexcept;
		[[nodiscard]] const Prefab& operator[](PrefabId id) const noexcept { return m_prefabs[id]; }
		[[nodiscard]] u32 count() const noexcept { return static_cast<u32>(m_prefabs.size()); }

		[[nodiscard]] const Components& components() const noexcept { return m_components; }

	private:
		const Components& m_components;
		Vector<Prefab> m_prefabs{&memory::heap(MemoryTag::ECS)};
	};
}
