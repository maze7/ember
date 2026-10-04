#pragma once

#include <ember/anim/components.h>
#include <ember/audio/components.h>
#include <ember/audio/engine.h>
#include <ember/ecs/system.h>

#include <utility>

namespace ember::audio
{
	/**
	 * A world's sound. What its emitters ask for goes through here to the engine, and everything it
	 * holds stops with the world: an editor's Stop is its session going away, and the sound goes with it.
	 * A system that sees something happen plays it here directly.
	 */
	class Sounds final
	{
	public:
		explicit Sounds(Engine& engine) noexcept : m_engine(&engine) {}
		~Sounds() noexcept;

		Sounds(const Sounds&)			 = delete;
		Sounds& operator=(const Sounds&) = delete;

		/** A sound at a place, now. */
		void play(Event event, glm::vec2 position, Span<const ParamValue> params = {}) noexcept
		{
			m_engine->play(event, position, params);
		}

		/** A sound with no place, now. */
		void play(Event event, Span<const ParamValue> params = {}) noexcept { m_engine->play(event, params); }

		/** A sustained sound this world holds until it stops it, or goes. */
		[[nodiscard]] Voice start(Event event, glm::vec2 position, Span<const ParamValue> params = {}) noexcept;
		void stop(Voice voice, bool fade = true) noexcept;

		/** An entity's emitter is going: what it holds stops. The world tells it. */
		void forget(const Emitter& emitter) noexcept;

		/** Whether a bank has gone since this was last asked: its sounds stopped, and are asked for still. */
		[[nodiscard]] bool bank_went() noexcept
		{
			const u32 unloads = m_engine->unloads();
			return std::exchange(m_unloads, unloads) != unloads;
		}

		[[nodiscard]] Engine& engine() noexcept { return *m_engine; }

	private:
		Engine* m_engine;
		Vector<Voice> m_held{&memory::heap(MemoryTag::Audio)};
		u32 m_unloads = 0;
	};

	namespace detail
	{
		template <class Member> struct OwnerOf;
		template <class C> struct OwnerOf<glm::vec2 C::*>
		{
			using type = C;
		};
	}

	/** The component a game keeps positions in, from a pointer to its vec2: &Position::value. */
	template <auto Position> using Positioned = typename detail::OwnerOf<decltype(Position)>::type;

	/** Present: every emitter where its entity is drawn this frame. */
	template <auto Position> void place(ecs::View<const Positioned<Position>, Emitter> emitters)
	{
		for (auto [entity, position, emitter] : emitters.each())
			emitter.position = position.*Position;
	}

	/** Present: the sounds each entity's animations passed this frame, asked of its emitter. */
	void cue(ecs::View<const anim::Pose, Emitter> posed, const anim::Clock& clock);

	/**
	 * Present: what every emitter was asked for, played. Sounds whose moment has come start, unless it
	 * is long gone; holds start, follow their entity and stop; parameters reach what plays. This is the
	 * only system that talks to the engine.
	 */
	void emit(ecs::View<Emitter> emitters, Sounds& sounds, const anim::Clock& clock);

	/**
	 * The component and the three systems. Register it after the systems that ask for sounds, and after
	 * anim::register_systems, whose poses carry the sounds animations make. Position says where the game
	 * keeps an entity's position:
	 *
	 *     audio::register_systems<&Position::value>(registry);
	 */
	template <auto Position> void register_systems(ecs::Registry& registry) noexcept
	{
		registry.components<Emitter>();
		registry.template present<&place<Position>>();
		registry.template present<&cue>();
		registry.template present<&emit>();
	}

	/** A world's sounds, played through the engine, which outlives the world. */
	void add_resources(ecs::World& world, Engine& engine) noexcept;

	/** A sound asked for later than this after its moment is not played: an entity met mid-swing is not heard swinging.
	 */
	inline constexpr f64 TOO_LATE = 0.25;
}
