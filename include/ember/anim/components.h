#pragma once

#include <ember/core/common.h>
#include <ember/core/hash.h>
#include <ember/ecs/component.h>
#include <ember/net/serialize.h>

#include <glm/vec2.hpp>

/**
 * An entity's animation as three components: what gameplay asks of it (Animator), what fills its
 * rig's slots (Look), and what the client sampled from the two this frame (Pose). Plain data, so
 * prefabs carry them, and a server, which never animates, includes this header and links nothing.
 * Ember::Anim loads the files they name and samples them.
 */
namespace ember::anim
{
	/** A name from an animation file as a number: a slot, layer, clip, sprite, event, input or point. */
	using Name = u64;

	/** One name inside another: a mounted rig's slot inside the mount, a layer's point inside the layer. */
	[[nodiscard]] constexpr Name join(Name outer, Name inner) noexcept
	{
		return outer ^ (inner + 0x9e3779b97f4a7c15ull + (outer << 6) + (outer >> 2));
	}

	/** A name's number. A dotted name joins its parts: name("weapon.blade") is join(name("weapon"), name("blade")). */
	[[nodiscard]] constexpr Name name(StringView text) noexcept
	{
		Name result	 = 0;
		size_t start = 0;

		for (size_t i = 0; i <= text.size(); ++i)
		{
			if (i < text.size() && text[i] != '.')
				continue;

			const Name part = hash_text(text.substr(start, i - start));
			result			= start == 0 ? part : join(result, part);
			start			= i + 1;
		}

		return result;
	}

	/** An id the library has not handed out. */
	inline constexpr u16 NO_ID = 0xFFFF;

	/**
	 * What fills an entity's slots: a sheet in a slot whose layer draws one, or a rig mounted in a
	 * slot whose layer sits on a socket. A mounted rig's own slots are named through the mount,
	 * "weapon.blade", and a slot the look leaves alone shows the rig's default. Paths are asset
	 * names that outlive the entity, as string literals and item tables do.
	 */
	struct Look
	{
		struct Slot
		{
			Name name		  = 0;
			const char* asset = nullptr; // a .sheet, or a .anim when `rig`
			bool rig		  = false;
			u16 id			  = NO_ID; // the library's, once resolve() has been
		};

		static constexpr u32 CAPACITY = 6; // a paper doll and its weapon

		Slot slots[CAPACITY] = {};
		u8 count			 = 0;

		/** A sheet in a slot. Asking again for what it holds changes nothing. */
		constexpr void set(StringView slot, const char* sheet) noexcept { put(name(slot), sheet, false); }

		/** A rig mounted in a slot. */
		constexpr void mount(StringView slot, const char* rig) noexcept { put(name(slot), rig, true); }

		/** Empties a slot, so the rig's default shows again. */
		constexpr void clear(StringView slot) noexcept
		{
			if (const Slot* found = find(name(slot)))
				slots[found - slots] = slots[--count];
		}

		[[nodiscard]] constexpr const Slot* find(Name slot) const noexcept
		{
			for (u32 i = 0; i < count; ++i)
				if (slots[i].name == slot)
					return &slots[i];
			return nullptr;
		}

	private:
		constexpr void put(Name slot, const char* asset, bool rig) noexcept
		{
			const Slot* found = find(slot);
			if (found == nullptr && count == CAPACITY)
				return;

			Slot& entry = found != nullptr ? slots[found - slots] : slots[count++];
			if (entry.name == slot && entry.asset == asset && entry.rig == rig)
				return;

			entry = {.name = slot, .asset = asset, .rig = rig};
		}
	};

	EMBER_COMPONENT(Look, Client);

	/** How a clip plays: from when, on the animation clock in seconds, and how fast. */
	struct Play
	{
		f64 since = 0.0;
		f32 speed = 1.0f;
	};

	/**
	 * What gameplay asks of an entity's animation: a clip for its rig and for each rig mounted in it,
	 * overlays over them, the inputs its rigs read, which way it faces, how far it leans and when a
	 * hitstop holds it. Asking for what already plays changes nothing, so a Present system says what
	 * should play from sim state alone, every frame, and remembers nothing.
	 */
	struct Animator
	{
		/** A clip as it was asked for. */
		struct Played
		{
			Name clip	= 0;
			f64 since	= 0.0;
			f32 speed	= 1.0f;
			f32 seconds = 0.0f; // an overlay's: how long its whole clip plays for, or 0 for the clip's own length
		};

		/** What one rig plays: the entity's own, slot 0, or the rig mounted in a slot. */
		struct Playhead
		{
			/** What played before, newest first: a key of "current" starts where the clip it replaced was. */
			static constexpr u32 HISTORY = 3;

			Name slot				 = 0;
			Played now				 = {};
			Played previous[HISTORY] = {};
		};

		struct Input
		{
			Name name = 0;
			f32 value = 0.0f;
		};

		static constexpr u32 PLAYHEADS = 3;
		static constexpr u32 OVERLAYS  = 2;
		static constexpr u32 INPUTS	   = 4;

		const char* rig = nullptr; // "anims/human.anim", for the entity's life
		u16 rig_id		= NO_ID;   // the library's, once resolve() has been

		Playhead playheads[PLAYHEADS] = {};
		u8 playhead_count			  = 1;
		Played overlays[OVERLAYS]	  = {};
		u8 overlay_count			  = 0;
		Input inputs[INPUTS]		  = {};
		u8 input_count				  = 0;
		glm::vec2 facing			  = {1.0f, 0.0f};
		f32 lean	   = 0.0f; // radians, clockwise on screen whichever way it faces: tipped about its origin
		f64 held_from  = 0.0;  // a hitstop, on the animation clock: see hold()
		f64 held_until = 0.0;
		glm::vec2 squashed = {1.0f, 1.0f}; // a squash: the scale it starts at, easing back to 1 over squash_seconds
		f64 squash_from	   = 0.0;
		f32 squash_seconds = 0.0f;

		/** The entity's own rig plays a clip. */
		void play(StringView clip, Play how = {}) noexcept { start(playhead(0), name(clip), how); }

		/** The same, by a clip's number: what a Playing component carries. */
		void play(Name clip, Play how = {}) noexcept { start(playhead(0), clip, how); }

		/** The rig mounted in a slot plays a clip. */
		void play(StringView slot, StringView clip, Play how = {}) noexcept
		{
			start(playhead(name(slot)), name(clip), how);
		}

		/**
		 * A clip over what the entity's rig plays, from `since` until it ends: offsets and turns add,
		 * scales multiply, flashes take the brighter. Ask with when it last began; one that has run
		 * its course draws nothing. Given `seconds`, the whole clip plays over that long instead of its
		 * own length: a squash as long as the dash it is drawn for, however that is tuned.
		 */
		void overlay(StringView clip, f64 since, f32 seconds = 0.0f) noexcept
		{
			const Name key = name(clip);
			for (u32 i = 0; i < overlay_count; ++i)
			{
				if (overlays[i].clip == key)
				{
					overlays[i].since	= since;
					overlays[i].seconds = seconds;
					return;
				}
			}

			if (overlay_count < OVERLAYS)
				overlays[overlay_count++] = {.clip = key, .since = since, .seconds = seconds};
		}

		/** Which way the entity faces. A rig drawn facing east draws west as its mirror image. */
		void face(glm::vec2 direction) noexcept
		{
			if (direction.x != 0.0f || direction.y != 0.0f)
				facing = direction;
		}

		/**
		 * A hitstop: from `from` until `until` on the animation clock, what it plays that began before
		 * `from` stands still, and then plays on from where it stood, as much later as it was held. What
		 * begins at `from` or after plays as it would: a flash for the blow. A later hold takes its place.
		 */
		void hold(f64 from, f64 until) noexcept
		{
			held_from  = from;
			held_until = until;
		}

		/**
		 * A squash from `since`: every part scaled by `scale`, easing back to its own size over `seconds`.
		 * A later squash takes the place of an earlier one.
		 */
		void squash(glm::vec2 scale, f64 since, f32 seconds) noexcept
		{
			squashed	   = scale;
			squash_from	   = since;
			squash_seconds = seconds;
		}

		/** The squash's scale at a moment: 1 before it, after it, and when there is none. */
		[[nodiscard]] glm::vec2 squash_at(f64 now) const noexcept
		{
			if (squash_seconds <= 0.0f || now < squash_from)
				return {1.0f, 1.0f};
			const f32 t = static_cast<f32>((now - squash_from) / static_cast<f64>(squash_seconds));
			if (t >= 1.0f)
				return {1.0f, 1.0f};
			const f32 left = (1.0f - t) * (1.0f - t);
			return {1.0f + (squashed.x - 1.0f) * left, 1.0f + (squashed.y - 1.0f) * left};
		}

		/** A number the rigs read: the aim a weapon turns to, in radians, clockwise on screen. */
		void set(StringView input, f32 value) noexcept
		{
			const Name key = name(input);
			for (u32 i = 0; i < input_count; ++i)
			{
				if (inputs[i].name == key)
				{
					inputs[i].value = value;
					return;
				}
			}

			if (input_count < INPUTS)
				inputs[input_count++] = {.name = key, .value = value};
		}

		[[nodiscard]] f32 input(Name key) const noexcept
		{
			for (u32 i = 0; i < input_count; ++i)
				if (inputs[i].name == key)
					return inputs[i].value;
			return 0.0f;
		}

		[[nodiscard]] const Playhead* find(Name slot) const noexcept
		{
			for (u32 i = 0; i < playhead_count; ++i)
				if (playheads[i].slot == slot)
					return &playheads[i];
			return nullptr;
		}

	private:
		[[nodiscard]] Playhead& playhead(Name slot) noexcept
		{
			if (const Playhead* found = find(slot))
				return playheads[found - playheads];

			Playhead& added = playheads[playhead_count < PLAYHEADS ? playhead_count++ : PLAYHEADS - 1];
			added			= {.slot = slot};
			return added;
		}

		static void start(Playhead& head, Name clip, Play how) noexcept
		{
			if (head.now.clip == clip && head.now.since == how.since && head.now.speed == how.speed)
				return;

			for (u32 i = Playhead::HISTORY - 1; i > 0; --i)
				head.previous[i] = head.previous[i - 1];
			head.previous[0] = head.now;
			head.now		 = {.clip = clip, .since = how.since, .speed = how.speed};
		}
	};

	EMBER_COMPONENT(Animator, Client);

	/**
	 * What gameplay asked an entity's rig to play, as every machine knows it: a clip by its number,
	 * from a tick. The server's brains write it (a script's e:play("squat")), it replicates, and a
	 * client's Present system plays it on the Animator from the moment that tick is drawn. Nothing
	 * while clip is 0.
	 */
	struct Playing
	{
		Name clip = 0;
		u32 since = 0; // the tick it started

		template <class Stream> bool serialize(Stream& stream)
		{
			u32 low	 = static_cast<u32>(clip);
			u32 high = static_cast<u32>(clip >> 32);
			serialize_bits(stream, low, 32);
			serialize_bits(stream, high, 32);
			serialize_bits(stream, since, 32);
			if (Stream::IsReading)
				clip = (static_cast<Name>(high) << 32) | low;
			return true;
		}
	};

	EMBER_COMPONENT(Playing, Replicated);

	/**
	 * An entity's animation as sampled this frame: the parts it draws, its named points, the events it
	 * passed and what its rigs say those sound like.
	 */
	struct Pose
	{
		struct Part
		{
			u16 sheet		   = NO_ID;
			u16 sprite		   = 0;
			glm::vec2 position = {};   // the sprite's pivot, in texels from the entity: x right, y down the screen
			f32 angle		   = 0.0f; // radians, clockwise on screen
			glm::vec2 scale	   = {1.0f, 1.0f};
			f32 flash		   = 0.0f;	// 0 the art, 1 white
			f32 order		   = 0.0f;	// back to front
			bool mirror		   = false; // left for right
		};

		struct Point
		{
			Name name		   = 0; // "body.heel"
			glm::vec2 position = {};
		};

		static constexpr u32 PARTS	= 8;
		static constexpr u32 POINTS = 6;
		static constexpr u32 EVENTS = 4;
		static constexpr u32 SOUNDS = 4;

		Part parts[PARTS]	 = {};
		u8 part_count		 = 0;
		Point points[POINTS] = {};
		u8 point_count		 = 0;
		Name events[EVENTS]	 = {}; // "step", or a mounted rig's "weapon.slash"
		u8 event_count		 = 0;
		u64 sounds[SOUNDS]	 = {}; // what its rigs say those events sound like: audio events, by their hashed paths
		u8 sound_count		 = 0;

		// The inputs it was sampled with: where the clips they play were, so the next frame fires what they pass.
		Animator::Input inputs[Animator::INPUTS] = {};
		u8 input_count							 = 0;

		[[nodiscard]] const glm::vec2* point(StringView point_name) const noexcept
		{
			const Name key = name(point_name);
			for (u32 i = 0; i < point_count; ++i)
				if (points[i].name == key)
					return &points[i].position;
			return nullptr;
		}

		[[nodiscard]] bool fired(StringView event) const noexcept
		{
			const Name key = name(event);
			for (u32 i = 0; i < event_count; ++i)
				if (events[i] == key)
					return true;
			return false;
		}
	};

	EMBER_COMPONENT(Pose, Client);

	/** The time a client presents on, in seconds: this frame's and the one before, so events fire once. */
	struct Clock
	{
		f64 now		 = 0.0;
		f64 previous = 0.0;

		void advance(f64 dt) noexcept { set(now + dt); }

		void set(f64 time) noexcept
		{
			previous = now;
			now		 = time;
		}
	};
}
