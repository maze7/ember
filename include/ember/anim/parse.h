#pragma once

#include <ember/anim/rig.h>
#include <ember/containers/span.h>

/**
 * Animation files from their text, which people write: comments and trailing commas allowed, and
 * every refusal says what to fix, with the line and column when the text will not parse.
 */
namespace ember::anim
{
	/** An image as a sheet measures it: RGBA8, row after row. */
	struct Image
	{
		Extent2D extent;
		Span<const u8> rgba;
	};

	/** The image a .sheet names, to load before parse_sheet() can measure it. */
	[[nodiscard]] bool sheet_image(StringView text, String& path, String& error) noexcept;

	/**
	 * A .sheet over its image:
	 *
	 *   {
	 *       "image": "textures/human.png",
	 *       "cell": [24, 24],               // cells numbered across, then down
	 *       "pivot": [12, 20],              // in each cell, where it stands
	 *       "points": { "hand": [12, 12] }, // in each cell, measured as the pivot is
	 *       "sprites": { "idle": [0, 1] },  // idle_0 and idle_1, and idle for the first
	 *       "shadow": false                 // casts none: true unless it says
	 *   }
	 */
	[[nodiscard]] bool parse_sheet(StringView text, const Image& image, Sheet& out, String& error) noexcept;

	/**
	 * A .anim:
	 *
	 *   {
	 *       "slots": { "body": "sheets/human.sheet" },
	 *       "sounds": { "step": "event:/Footstep" },
	 *       "turn": "aim",
	 *       "layers": [{ "name": "body" }, { "name": "weapon", "socket": "body.hand", "sort": "y" }],
	 *       "clips": {
	 *           "walk": { "loop": true, "frames": { "body": { "sprites": "walk", "ms": 200 } },
	 *                     "events": [[0, "step"]] },
	 *           "dash": { "tracks": { "body.scale": [[0, [1.6, 0.6]], [866, [1, 1], "expo_out"]] } },
	 *           "swing": { "base": "walk", "length": 300 }
	 *       }
	 *   }
	 *
	 * A layer's rest values sit beside its name ("x", "orbit", "scale"...), as do "slot", "sprite",
	 * "socket" (an earlier layer's point) and "sort". A clip with a "base" starts as a copy of that
	 * clip, which comes earlier in the file: what it says itself, a layer's frames, a track or its
	 * events, replaces the base's. "sounds" says what the rig's events sound like, each by the path of
	 * an audio event: whichever clip passes an event of that name, the entity's emitter plays it.
	 */
	[[nodiscard]] bool parse_rig(StringView text, Rig& out, String& error) noexcept;
}
