#pragma once

namespace Ember
{
	class Batcher;
	class Scene;

	class System
	{
	public:
		virtual ~System() = default;

		virtual void update_fixed(Scene& scene, double dt) {}
		virtual void update_variable(Scene& scene, double dt) {}
		virtual void render(Scene& scene, Batcher& batcher) {}
	};
}
