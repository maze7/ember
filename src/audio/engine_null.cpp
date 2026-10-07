#include <ember/audio/engine.h>

// The engine of a build without FMOD: the same calls, and nothing is heard. Games and tools are
// written once, and a machine without the SDK still builds them.
namespace ember::audio
{
	struct Engine::Impl
	{
	};

	Engine::Engine() noexcept  = default;
	Engine::~Engine() noexcept = default;

	bool Engine::init(const EngineDef&) noexcept { return false; }
	void Engine::shutdown() noexcept {}
	void Engine::update() noexcept {}

	bool Engine::load_bank(StringView, bool) noexcept { return false; }
	bool Engine::load_bank(StringView, Span<const u8>, bool) noexcept { return false; }
	void Engine::unload_bank(StringView) noexcept {}
	u32 Engine::unloads() const noexcept { return 0; }
	bool Engine::has(Event) const noexcept { return false; }
	f32 Engine::range(Event) const noexcept { return 0.0f; }

	void Engine::play(Event, Span<const ParamValue>) noexcept {}
	void Engine::play(Event, glm::vec2, Span<const ParamValue>) noexcept {}

	Voice Engine::start(Event, glm::vec2, Span<const ParamValue>) noexcept { return {}; }
	void Engine::stop(Voice, bool) noexcept {}
	void Engine::move(Voice, glm::vec2) noexcept {}
	void Engine::set(Voice, Span<const ParamValue>) noexcept {}
	bool Engine::playing(Voice) noexcept { return false; }

	void Engine::music(Event) noexcept {}
	void Engine::ambience(Event) noexcept {}
	void Engine::set_music(Param, f32) noexcept {}

	void Engine::set(Param, f32) noexcept {}
	void Engine::set_volume(StringView, f32) noexcept {}
	void Engine::set_paused(StringView, bool) noexcept {}
	void Engine::listen(const Listener&) noexcept {}

	const EngineDef& Engine::def() const noexcept
	{
		static const EngineDef none;
		return none;
	}

	Stats Engine::stats() const noexcept { return {}; }
}
