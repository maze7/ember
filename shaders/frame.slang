#language slang 2026

// What every scene pass reads: the frame's constants and the view the pass renders. Declared once,
// here, so the material API, the shading models and the entry points agree on one layout. Constant
// blocks follow std140, so members are grouped into 16 byte rows by hand, and the C++ mirror can
// static_assert its offsets row by row.
module frame;

import ember;

/// The scene's light, as a lighting feature publishes it for the frame. A struct of whole rows, so it
/// nests in FrameConstants at a 16 byte boundary with nothing to pad.
public struct LightingConstants
{
	float3 sky;         // hemisphere ambient, linear: the light arriving from straight up
	uint   light_count;

	float3 ground;      // and from straight down; directions in between blend the two
	uint   lights;      // LightData table

	uint  first_light;  // the frame's first record in that table
	uint  bands;        // Pixel model: steps per light, zero for smooth
	float dither;       // Pixel model: 0 rounds to the nearest step, 1 dithers between the two nearest
	float dither_cell;  // Pixel model: world units a dither cell spans, in the view's plane

	uint shadows;       // ShadowData table
	uint first_shadow;  // the frame's first record in that table
	uint _pad0;
	uint _pad1;
};

public struct FrameConstants
{
	float time;        // seconds, wrapped by the CPU every hour so float precision stays sub-millisecond
	float delta_time;
	uint  frame_index;
	uint  flags;       // engine features enabled this frame: branch on these, never permute

	// The scene's tables by bindless slot. Per frame because they move only when a table grows.
	uint objects;
	uint transforms;
	uint instances;
	uint material_keys;

	uint positions;
	uint attributes;
	uint geometries;
	uint _pad0;

	// The frame's, not the view's: every view of a frame sees the same lights.
	LightingConstants lighting;
};

public struct ViewConstants
{
	float4x4 view_projection;
	float4x4 view;
	float4x4 inverse_view_projection;

	// The eye in homogeneous coordinates: (position, 1) for a perspective view and (the direction
	// toward the camera, 0) for an orthographic one. normalize(eye.xyz - world * eye.w) is then the
	// view direction for both without a branch; the pixel-art camera is orthographic, and a
	// position-based view vector would bend its highlights across the screen.
	float4 eye;

	float2 resolution;
	float2 inverse_resolution;

	uint scene_color;     // the opaque world's colour and depth: valid in transparent and screen passes
	uint scene_depth;
	uint scene_sampler;   // linear clamp, for reading the two above
	uint _pad0;
};

public [[vk::binding(CONSTANTS_FRAME, SET_CONSTANTS)]] ConstantBuffer<FrameConstants> g_frame;
public [[vk::binding(CONSTANTS_PASS, SET_CONSTANTS)]]  ConstantBuffer<ViewConstants>  g_view;
