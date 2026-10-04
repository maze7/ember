#pragma once

#include <ember/audio/event.h>
#include <ember/ecs/component.h>

#include <glm/vec2.hpp>

/**
 * An entity's sound as one component: what gameplay asks it to make. Plain data, so prefabs carry it,
 * and a server, which makes no sound, includes this header and links nothing.
 */
namespace ember::audio
{
	/** A voice the world has not given out. */
	inline constexpr u32 NO_VOICE = 0;

	/**
	 * What gameplay asks of an entity's sound. A Present system says what it should sound like from sim
	 * state alone, every frame, and remembers nothing: asking again for what was asked changes nothing.
	 *
	 *   play(event, since)   it made this sound at that moment: heard once, however often it is said
	 *   hold(event)          it makes this sound for as long as it is said, and stops when it is not
	 *   set(param, value)    a number every sound it makes reads: the ground it walks on
	 *
	 * Moments are on the animation clock, in seconds, so a sound and the clip it belongs to start
	 * together. The sounds its animations name play through it too.
	 */
	struct Emitter
	{
		/** A sound made at a moment. */
		struct Shot
		{
			Event event;
			f64 since = 0.0;
			bool done = true; // played, or let go as too late
		};

		/** A sound that lasts while it is asked for. */
		struct Hold
		{
			Event event;
			u32 voice  = NO_VOICE; // the world's, while it plays
			bool asked = false;	   // this frame
			bool ended = false;	   // played out on its own; asking on does not start it again
		};

		static constexpr u32 SHOTS	= 4;
		static constexpr u32 HOLDS	= 2;
		static constexpr u32 PARAMS = 4;

		/** Moments closer than this are one act: a swing predicted a tick early, then put right. */
		static constexpr f64 SAME_ACT = 0.05;

		/** A sound it makes for as long as it lives: a fire's crackle. A prefab says so. */
		Event loop;

		/** Where it is, in world units: audio::place keeps it where the game keeps the entity. */
		glm::vec2 position = {};
		glm::vec2 placed   = {}; // where its held sounds were last put

		Shot shots[SHOTS]		  = {};
		Hold holds[HOLDS]		  = {};
		ParamValue params[PARAMS] = {};
		u8 shot_count			  = 0;
		u8 hold_count			  = 0;
		u8 param_count			  = 0;
		bool params_changed		  = false;

		/**
		 * The entity made this sound at `since`. Said again with the same moment it changes nothing, and
		 * with one a few ticks off it moves the moment and makes no second sound: a prediction put right.
		 */
		constexpr void play(Event event, f64 since) noexcept
		{
			for (u32 i = 0; i < shot_count; ++i)
			{
				Shot& shot = shots[i];
				if (shot.event != event)
					continue;

				// Later by more than a correction is a new act. Earlier by as much is one taken back, which
				// leaves the act before it: heard already. Between the two, the same act's moment, put right.
				const f64 later = since - shot.since;
				if (later >= SAME_ACT)
					shot.done = false;
				else if (later <= -SAME_ACT)
					shot.done = true;

				shot.since = since;
				return;
			}

			// A new sound takes a free place, else the place of the one heard longest ago.
			u32 place = shot_count;
			if (shot_count == SHOTS)
			{
				place = SHOTS;
				for (u32 i = 0; i < SHOTS; ++i)
					if (shots[i].done && (place == SHOTS || shots[i].since < shots[place].since))
						place = i;

				if (place == SHOTS)
					return;
			}
			else
			{
				++shot_count;
			}

			shots[place] = {.event = event, .since = since, .done = false};
		}

		/** The entity makes this sound now, and until a frame goes by without it being said. */
		constexpr void hold(Event event, bool on = true) noexcept
		{
			if (!on)
				return;

			for (u32 i = 0; i < hold_count; ++i)
			{
				if (holds[i].event == event)
				{
					holds[i].asked = true;
					return;
				}
			}

			if (hold_count < HOLDS)
				holds[hold_count++] = {.event = event, .asked = true};
		}

		/** A parameter of every sound the entity makes, those playing and those to come. */
		constexpr void set(Param param, f32 value) noexcept
		{
			for (u32 i = 0; i < param_count; ++i)
			{
				if (params[i].param == param)
				{
					params_changed	= params_changed || params[i].value != value;
					params[i].value = value;
					return;
				}
			}

			if (param_count < PARAMS)
			{
				params[param_count++] = {.param = param, .value = value};
				params_changed		  = true;
			}
		}
	};

	EMBER_COMPONENT(Emitter, Client);
}
