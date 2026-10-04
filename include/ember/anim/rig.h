#pragma once

#include <ember/anim/components.h>
#include <ember/core/common.h>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

/**
 * The two kinds of animation file, as the sampler reads them.
 *
 *   .sheet   an image cut into named sprites, with where each stands (its pivot) and named points
 *   .anim    a rig: layers that draw from slots, and clips that key the layers' channels
 *
 * A layer's channels are x, y, orbit and radius (where it sits), angle, scale, flash and show. Keys
 * are [ms, value, ease into this key]; a first key of "current" starts where the clip it replaced
 * was, so a swing cut short carries on. A flipbook steps a layer through a sheet's sequence. Units
 * are texels and degrees, y down the screen, angles clockwise.
 */
namespace ember::anim
{
	/** How a value arrives at a key from the one before: Robert Penner's curves, by game2's names. */
	enum class Ease : u8
	{
		Step,
		Linear,
		QuadIn,
		QuadOut,
		QuadInOut,
		CubicIn,
		CubicOut,
		CubicInOut,
		QuartIn,
		QuartOut,
		QuartInOut,
		SineIn,
		SineOut,
		SineInOut,
		ExpoIn,
		ExpoOut,
		ExpoInOut,
		BackIn,
		BackOut,
		BackInOut,
		ElasticOut,
		BounceOut,
		Count
	};

	/** Progress t in [0, 1] along a curve. */
	[[nodiscard]] f32 ease(Ease ease, f32 t) noexcept;

	/** One number of a layer. */
	enum class Channel : u8
	{
		X,
		Y,
		Orbit, // degrees around where the layer sits, `radius` out
		Radius,
		Angle, // degrees
		ScaleX,
		ScaleY,
		Flash, // 0 the art, 1 white
		Show,  // below a half, the layer draws nothing
		Count
	};

	inline constexpr u32 CHANNELS = static_cast<u32>(Channel::Count);

	/** A value by name, in a list short enough that a scan is the quickest search. */
	template <class T> struct Named
	{
		Name name = 0;
		T value	  = {};
	};

	template <class T> [[nodiscard]] const T* find(const Vector<Named<T>>& list, Name name) noexcept
	{
		for (const Named<T>& entry : list)
			if (entry.name == name)
				return &entry.value;
		return nullptr;
	}

	// --- .sheet ----------------------------------------------------------------------------------

	struct Sprite
	{
		glm::vec2 min	 = {}; // texels on the image
		glm::vec2 size	 = {};
		glm::vec2 pivot	 = {}; // from min: the texel corner that stands where its layer is
		glm::vec4 opaque = {}; // from min: the box its opaque texels fill, x0 y0 x1 y1
	};

	/** Frames of one name: "walk" is walk_0, walk_1... */
	struct Sequence
	{
		u16 first = 0;
		u16 count = 0;
	};

	/** An image cut into sprites, which share the sheet's named points, measured like their pivots. */
	struct Sheet
	{
		Extent2D extent = {};
		Vector<Sprite> sprites;
		Vector<Named<u16>> names; // "walk_1", and "walk" for its sequence's first frame
		Vector<Named<Sequence>> sequences;
		Vector<Named<glm::vec2>> points;
		bool casts_shadow = true;
	};

	// --- .anim -----------------------------------------------------------------------------------

	struct Key
	{
		f32 ms			  = 0.0f;
		f32 value		  = 0.0f;
		Ease ease		  = Ease::Linear; // into this key, from the one before
		bool from_current = false;		  // a first key of "current"
	};

	/** One channel of one layer, its keys a run of the clip's. */
	struct Track
	{
		u16 layer		= 0;
		Channel channel = Channel::X;
		u32 first		= 0;
		u32 count		= 0;
	};

	struct SpriteKey
	{
		f32 ms		= 0.0f;
		Name sprite = 0;
	};

	/** The sprite a layer shows, stepping from key to key. */
	struct SpriteTrack
	{
		u16 layer = 0;
		u32 first = 0;
		u32 count = 0;
	};

	/** A layer stepping through a sheet's sequence, `ms` a frame, from `at`. */
	struct Flipbook
	{
		u16 layer	  = 0;
		Name sequence = 0;
		f32 ms		  = 100.0f;
		f32 at		  = 0.0f;
	};

	struct Event
	{
		f32 ms	  = 0.0f;
		Name name = 0;
	};

	/** Tracks in layer order, so sampling walks them once; events in time order. */
	struct Clip
	{
		Name name  = 0;
		f32 length = -1.0f; // ms; below zero, where the last key or flipbook frame ends
		bool loop  = false;
		Name input = 0;		// one that plays it in place of the clock: a walk by the distance walked
		f32 span   = 1.0f;	// how much of that input takes it from its start to its end

		Vector<Track> tracks;
		Vector<Key> keys;
		Vector<SpriteTrack> sprite_tracks;
		Vector<SpriteKey> sprite_keys;
		Vector<Flipbook> flipbooks;
		Vector<Event> events;
	};

	/** The most layers a rig has: a paper doll's and room to spare. The sampler keeps them on its stack. */
	inline constexpr u32 MAX_LAYERS = 16;

	struct Layer
	{
		Name name		   = 0;
		Name slot		   = 0;		// where its sheet comes from: the look, else the rig's default
		u16 parent		   = NO_ID; // the earlier layer whose sprite carries the socket
		Name socket		   = 0;		// that sprite's point, where this layer's space starts
		bool sort_y		   = false; // what it draws goes behind the layers before it while above its socket
		Name sprite		   = 0;		// what it shows when no clip says
		f32 rest[CHANNELS] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
	};

	struct SlotDefault
	{
		Name slot = 0;
		String sheet;
	};

	/** What an event sounds like: an audio event, by its path and by the hash audio knows it by. */
	struct Cue
	{
		u64 sound = 0;
		String path;
	};

	struct Rig
	{
		Vector<Layer> layers;
		Vector<SlotDefault> slots;

		/// What its events sound like, by the event's name: every clip's "step" sounds like this.
		Vector<Named<Cue>> sounds;

		Vector<Clip> clips; // the first plays when nothing asks for another
		Name turn = 0;		// an input the whole rig turns by: a weapon by its aim

		/** The clip of that name, or null. */
		[[nodiscard]] const Clip* clip(Name name) const noexcept
		{
			for (const Clip& clip : clips)
				if (clip.name == name)
					return &clip;
			return nullptr;
		}

		/** What a playhead plays: the clip it asks for, or the first when it asks for none. */
		[[nodiscard]] const Clip* playing(Name name) const noexcept
		{
			return name != 0 ? clip(name) : (clips.empty() ? nullptr : &clips.front());
		}

		[[nodiscard]] const char* default_sheet(Name slot) const noexcept
		{
			for (const SlotDefault& entry : slots)
				if (entry.slot == slot)
					return entry.sheet.c_str();
			return nullptr;
		}
	};
}
