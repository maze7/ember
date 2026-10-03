#pragma once

#include <ember/core/common.h>
#include <ember/core/hash.h>
#include <ember/ecs/component.h>

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
	 * overlays over them, the inputs its rigs read, and which way it faces. Asking for what already
	 * plays changes nothing, so a Present system says what should play from sim state alone, every
	 * frame, and remembers nothing.
	 */
	struct Animator
	{
		/** A clip as it was asked for. */
		struct Played
		{
			Name clip = 0;
			f64 since = 0.0;
			f32 speed = 1.0f;
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

		/** The entity's own rig plays a clip. */
		void play(StringView clip, Play how = {}) noexcept { start(playhead(0), name(clip), how); }

		/** The rig mounted in a slot plays a clip. */
		void play(StringView slot, StringView clip, Play how = {}) noexcept
		{
			start(playhead(name(slot)), name(clip), how);
		}

		/**
		 * A clip over what the entity's rig plays, from `since` until it ends: offsets and turns add,
		 * scales multiply, flashes take the brighter. Ask with when it last began; one that has run
		 * its course draws nothing.
		 */
		void overlay(StringView clip, f64 since) noexcept
		{
			const Name key = name(clip);
			for (u32 i = 0; i < overlay_count; ++i)
			{
				if (overlays[i].clip == key)
				{
					overlays[i].since = since;
					return;
				}
			}

			if (overlay_count < OVERLAYS)
				overlays[overlay_count++] = {.clip = key, .since = since};
		}

		/** Which way the entity faces. A rig drawn facing east draws west as its mirror image. */
		void face(glm::vec2 direction) noexcept
		{
			if (direction.x != 0.0f || direction.y != 0.0f)
				facing = direction;
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

	/** An entity's animation as sampled this frame: the parts it draws, its named points and the events it passed. */
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

		Part parts[PARTS]	 = {};
		u8 part_count		 = 0;
		Point points[POINTS] = {};
		u8 point_count		 = 0;
		Name events[EVENTS]	 = {}; // "step", or a mounted rig's "weapon.slash"
		u8 event_count		 = 0;

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
}
