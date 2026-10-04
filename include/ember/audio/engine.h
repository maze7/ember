#pragma once

#include <ember/audio/event.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/memory/unique.h>

#include <glm/vec2.hpp>

/**
 * Ember::Audio plays what a sound designer makes in FMOD Studio. The designer owns the sounds, how
 * they are mixed and how music moves; the game says which events happen, where, and with what
 * parameters. This is the one object that talks to FMOD: a world plays through it (systems.h), and an
 * app uses it directly for what belongs to no world: menus, music, the mix.
 *
 * A build without FMOD (EMBER_ENABLE_AUDIO off) has the same engine, and every call does nothing.
 *
 * THREADING
 *   Any one thread at a time may call it; update() once a frame, after the frame's sounds are asked
 *   for. FMOD does its own work on its own threads: a call here is a command queued for them, and
 *   update() hands the frame's commands over together, so a sound and the listener it is heard from
 *   never land a frame apart.
 */
namespace ember::audio
{
	enum class Output : u8
	{
		Device,	 // the system's default output; None when it has none
		None,	 // nowhere: everything runs, nothing is heard
		Stepped, // nowhere, and FMOD runs on the caller, a mix block an update(): tests hear exactly what they asked
				 // for
		Count
	};

	/** Live Update is only on by default in dev builds. */
#if defined(NDEBUG)
	inline constexpr bool LIVE_UPDATE_DEFAULT = false;
#else
	inline constexpr bool LIVE_UPDATE_DEFAULT = true;
#endif

	struct EngineDef
	{
		Output output = Output::Device;

		/** Sounds mixed at once; past these the quietest are tracked but not mixed, until they are louder. */
		u32 voices		   = 64;
		u32 virtual_voices = 512;

		/** Sustained sounds a game holds at once: loops, music, ambience. */
		u32 held = 128;

		/**
		 * World units to one of Studio's distance units. At 1 a designer's ranges are in the world's own
		 * units; a game that passes its tile's size has them in tiles.
		 */
		f32 unit = 1.0f;

		/**
		 * One-shots a frame may start, and how many of those may be one event: a crowd of the same sound
		 * is no louder, only costlier, and FMOD's own threads fall behind past a thousand or so at once.
		 */
		u32 max_starts = 32;
		u32 max_same   = 3;

		/**
		 * A one-shot this much of its event's range past it, from where the listener fades sounds, is not
		 * started at all. See Engine::range() for the events this can judge.
		 */
		f32 cull_margin = 0.1f;

		/**
		 * FMOD's queue of a frame's commands: a one-shot is about 360 bytes of it. A full queue holds
		 * the caller until FMOD has caught up, which Stats::queue_stalls counts.
		 */
		u32 command_bytes = 256 * 1024;

		/** Lets FMOD Studio connect to the running game, to mix it and to profile it. */
		bool live_update = LIVE_UPDATE_DEFAULT;
	};

	/** A sustained sound: a loop an entity makes, a held note. Stale once it has stopped. */
	struct Voice
	{
		u32 bits = 0;

		[[nodiscard]] constexpr explicit operator bool() const noexcept { return bits != 0; }
		[[nodiscard]] friend constexpr bool operator==(Voice, Voice) noexcept = default;
	};

	/**
	 * Where the game is heard from, in world units. Sounds pan by where they are across the screen and
	 * fade by how far they are from the focus, so what is near the player is loud wherever the camera leans.
	 */
	struct Listener
	{
		glm::vec2 position = {};	 // the ground under the middle of the screen
		glm::vec2 focus	   = {};	 // the point sounds fade from: the player, as a rule
		f32 reach		   = 320.0f; // this far left or right of the middle, a sound is wholly in one speaker
	};

	/** A frame's numbers, for a panel. */
	struct Stats
	{
		u32 banks	= 0; // loaded
		u32 loading = 0; // banks still on their way
		u32 events	= 0; // known, from the banks loaded
		u32 held	= 0; // sustained sounds playing
		u32 tracks	= 0; // of music and ambience, playing
		u32 started = 0; // one-shots this frame
		u32 culled	= 0; // not started: out of earshot
		u32 dropped = 0; // not started: the frame's budget was spent
		u32 unknown = 0; // asked for and in no loaded bank

		u32 queue_peak	 = 0;	 // bytes of commands the fullest frame queued
		u32 queue_stalls = 0;	 // times a caller waited on a full queue: raise command_bytes
		f32 mixer_cpu	 = 0.0f; // percent of a core, FMOD's own threads
		f32 studio_cpu	 = 0.0f;
		u32 memory_bytes = 0;
	};

	class Engine final
	{
	public:
		Engine() noexcept;
		~Engine() noexcept;

		Engine(const Engine&)			 = delete;
		Engine& operator=(const Engine&) = delete;

		/** False, logged, when FMOD cannot start; every call then does nothing, and the game runs on. */
		bool init(const EngineDef& def = {}) noexcept;
		void shutdown() noexcept;

		/** Once a frame: the frame's commands go to FMOD together, and banks that arrived are taken in. */
		void update() noexcept;

		/**
		 * A bank Studio built, by its file. It loads on FMOD's own threads and its events play once it is
		 * in, a few frames on; whatever is asked for meanwhile is asked for again, so nothing waits for
		 * it. Events are found by their paths, which the strings bank holds: load it too. Loading a file
		 * again reloads it: what plays from the old bank stops, and what is asked for still starts again.
		 */
		bool load_bank(StringView file, bool preload = true) noexcept;
		void unload_bank(StringView file) noexcept;

		/**
		 * How many times a bank has been let go, reloads included. What played from it went with it, so
		 * whoever holds a sound that stopped compares this to tell one that played out from one whose
		 * bank went from under it.
		 */
		[[nodiscard]] u32 unloads() const noexcept;

		/** Whether a loaded bank has the event. */
		[[nodiscard]] bool has(Event event) const noexcept;

		/**
		 * How far an event is heard, in world units, counting what its spatialisers override its range
		 * to. Zero when it has no place, is unknown, or its bank does not say: one built by a Studio
		 * older than 2.04, whose events are never judged out of earshot.
		 */
		[[nodiscard]] f32 range(Event event) const noexcept;

		/** A sound with no place: an interface click, a stinger. */
		void play(Event event, Span<const ParamValue> params = {}) noexcept;

		/** A sound at a place in the world, there for as long as it lasts. */
		void play(Event event, glm::vec2 position, Span<const ParamValue> params = {}) noexcept;

		/** A sound that lasts until it is stopped. Null when the event is unknown or every hold is taken. */
		[[nodiscard]] Voice start(Event event, glm::vec2 position, Span<const ParamValue> params = {}) noexcept;

		/** Ends it, by the fade its event was given or at once. A stale voice is ignored, here and below. */
		void stop(Voice voice, bool fade = true) noexcept;
		void move(Voice voice, glm::vec2 position) noexcept;
		void set(Voice voice, Span<const ParamValue> params) noexcept;

		/** False once it has played out or was stopped, and the voice is stale from then on. */
		[[nodiscard]] bool playing(Voice voice) noexcept;

		/**
		 * The music, and the bed of ambience under it: each one event at a time, fading to the next as
		 * Studio has them fade. Asking for what plays changes nothing; asking for none stops it. One
		 * asked for before its bank arrives starts when it does.
		 */
		void music(Event event) noexcept;
		void ambience(Event event) noexcept;
		void set_music(Param param, f32 value) noexcept;

		/** A parameter every event shares: the time of day. */
		void set(Param global, f32 value) noexcept;

		/** A bus's or a VCA's level, "bus:/SFX" or "vca:/Music": 1 as mixed, 0 silent. */
		void set_volume(StringView path, f32 volume) noexcept;
		void set_paused(StringView bus, bool paused) noexcept;

		void listen(const Listener& listener) noexcept;

		[[nodiscard]] const EngineDef& def() const noexcept;
		[[nodiscard]] Stats stats() const noexcept;

	private:
		struct Impl;
		Unique<Impl> m_impl;
	};
}
