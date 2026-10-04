#include <ember/audio/systems.h>

#include <ember/core/profile.h>

#include <algorithm>

namespace ember::audio
{
	Sounds::~Sounds() noexcept
	{
		for (const Voice voice : m_held)
			m_engine->stop(voice, false);
	}

	Voice Sounds::start(Event event, glm::vec2 position, Span<const ParamValue> params) noexcept
	{
		const Voice voice = m_engine->start(event, position, params);
		if (voice)
			m_held.push_back(voice);
		return voice;
	}

	void Sounds::stop(Voice voice, bool fade) noexcept
	{
		m_engine->stop(voice, fade);

		if (const auto found = std::find(m_held.begin(), m_held.end(), voice); found != m_held.end())
		{
			*found = m_held.back();
			m_held.pop_back();
		}
	}

	void Sounds::forget(const Emitter& emitter) noexcept
	{
		for (u32 i = 0; i < emitter.hold_count; ++i)
			if (emitter.holds[i].voice != NO_VOICE)
				stop({.bits = emitter.holds[i].voice});
	}

	void cue(ecs::View<const anim::Pose, Emitter> posed, const anim::Clock& clock)
	{
		EMBER_PROFILE_FIBER_SCOPE_C("audio::cue", PROFILE_COLOR_AUDIO);
		for (auto [entity, pose, emitter] : posed.each())
			for (u32 i = 0; i < pose.sound_count; ++i)
				emitter.play(Event::of(pose.sounds[i]), clock.now);
	}

	void emit(ecs::View<Emitter> emitters, Sounds& sounds, const anim::Clock& clock)
	{
		EMBER_PROFILE_FIBER_SCOPE_C("audio::emit", PROFILE_COLOR_AUDIO);
		Engine& engine = sounds.engine();

		// A bank built again under the game took what played from it. Those sounds did not play out: they
		// are asked for still, and start again when the bank is back.
		const bool reloaded = sounds.bank_went();

		for (auto [entity, emitter] : emitters.each())
		{
			const Span<const ParamValue> params(emitter.params, emitter.param_count);

			for (u32 i = 0; i < emitter.shot_count; ++i)
			{
				Emitter::Shot& shot = emitter.shots[i];
				if (shot.done || clock.now < shot.since)
					continue;

				if (clock.now - shot.since <= TOO_LATE)
					engine.play(shot.event, emitter.position, params);
				shot.done = true;
			}

			if (emitter.loop)
				emitter.hold(emitter.loop);

			const bool moved = emitter.position != emitter.placed;
			emitter.placed	 = emitter.position;

			for (u32 i = 0; i < emitter.hold_count;)
			{
				Emitter::Hold& hold = emitter.holds[i];

				// Not asked for this frame: it stops, and its place is free.
				if (!hold.asked)
				{
					if (hold.voice != NO_VOICE)
						sounds.stop({.bits = hold.voice});
					hold = emitter.holds[--emitter.hold_count];
					continue;
				}

				hold.asked = false;
				hold.ended = hold.ended && !reloaded;
				++i;

				if (hold.voice == NO_VOICE)
				{
					// One the engine cannot start yet, its bank still loading, is asked for again next frame.
					if (!hold.ended)
						hold.voice = sounds.start(hold.event, emitter.position, params).bits;
					continue;
				}

				const Voice voice{.bits = hold.voice};
				if (!engine.playing(voice))
				{
					sounds.stop(voice);
					hold.voice = NO_VOICE;
					hold.ended = !reloaded;
					continue;
				}

				if (moved)
					engine.move(voice, emitter.position);
				if (emitter.params_changed)
					engine.set(voice, params);
			}

			emitter.params_changed = false;
		}
	}

	namespace
	{
		void emitter_going(Sounds& sounds, entt::registry& registry, entt::entity entity)
		{
			sounds.forget(registry.get<Emitter>(entity));
		}
	}

	void add_resources(ecs::World& world, Engine& engine) noexcept
	{
		Sounds& sounds = world.add_resource<Sounds>(engine);

		// An entity that goes, or loses its emitter, takes its held sounds with it.
		world.registry.on_destroy<Emitter>().connect<&emitter_going>(sounds);
	}
}
