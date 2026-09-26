#pragma once
#include "render.slang"

// The surface domain's authoring contract.
// A material is `struct Material : IMaterial` in one file that includes this prelude.
// The cooker appends surface_entry.slang; the author never writes an entry point, a binding
// or a push constant, and may not name anything ember_* / EMBER_*.

// ---- Authoring attributes (reflection only) ---------------------------------------------------
[__AttributeUsage(_AttributeTargets.Struct)] struct QueueAttribute      { string name; }; // opaque | cutout | transparent
[__AttributeUsage(_AttributeTargets.Struct)] struct CullAttribute       { string mode; }; // back | none | front
[__AttributeUsage(_AttributeTargets.Struct)] struct BlendAttribute      { string mode; }; // opaque | alpha | additive | premultiplied
[__AttributeUsage(_AttributeTargets.Struct)] struct DepthWriteAttribute { bool enabled; };
[__AttributeUsage(_AttributeTargets.Struct)] struct ShadowAttribute     { bool casts; };  // default true
[__AttributeUsage(_AttributeTargets.Struct)] struct PriorityAttribute   { int order; };   // draw order inside a queue
[__AttributeUsage(_AttributeTargets.Struct)] struct CapacityAttribute   { int records; }; // materials of this type, default 256

[__AttributeUsage(_AttributeTargets.Var)] struct DisplayAttribute { string name; };
[__AttributeUsage(_AttributeTargets.Var)] struct RangeAttribute   { float min; float max; };
[__AttributeUsage(_AttributeTargets.Var)] struct ColorAttribute   { bool hdr; };
[__AttributeUsage(_AttributeTargets.Var)] struct DefaultAttribute { string value; };   // "0.5", "1 1 1 1", "white", "normal"
[__AttributeUsage(_AttributeTargets.Var)] struct FilterAttribute  { string mode; };    // point | linear
[__AttributeUsage(_AttributeTargets.Var)] struct WrapAttribute    { string mode; };    // repeat | mirror | clamp | border
[__AttributeUsage(_AttributeTargets.Var)] struct SrgbAttribute    { bool linearize; }; // textures: colour data (default) or data

// ---- Parameter types: a bindless index pair, 8 bytes in the record ----------------------------
struct Texture2DRef
{
	uint texture;
	uint sampler;
	float4 sample(float2 uv)                    { return EMBER_TEX2D_NU(texture).Sample(EMBER_SAMPLER_NU(sampler), uv); }
	float4 sample_level(float2 uv, float level) { return EMBER_TEX2D_NU(texture).SampleLevel(EMBER_SAMPLER_NU(sampler), uv, level); }
	float4 sample_grad(float2 uv, float2 dx, float2 dy) { return EMBER_TEX2D_NU(texture).SampleGrad(EMBER_SAMPLER_NU(sampler), uv, dx, dy); }
};

struct TextureCubeRef
{
	uint texture;
	uint sampler;
	float4 sample(float3 dir)                    { return EMBER_TEXCUBE_NU(texture).Sample(EMBER_SAMPLER_NU(sampler), dir); }
	float4 sample_level(float3 dir, float level) { return EMBER_TEXCUBE_NU(texture).SampleLevel(EMBER_SAMPLER_NU(sampler), dir, level); }
};

struct Texture2DArrayRef
{
	uint texture;
	uint sampler;
	float4 sample(float2 uv, float layer) { return EMBER_TEX2D_ARRAY_NU(texture).Sample(EMBER_SAMPLER_NU(sampler), float3(uv, layer)); }
};

// ---- Constants a material may read ----------------------------------------------------------
struct FrameConstants
{
	float time;
	float dt;
	uint  frame_index;
	uint  flags;      // engine features on this frame: branch uniformly, never permute
};

struct ViewConstants
{
	float4x4 view_proj;
	float4x4 view;
	float4x4 inv_view_proj;
	float3   camera_position;
	float    near_plane;
	float2   resolution;
	float2   inv_resolution;
	uint     lights;        // bindless table of LightData, filled by the lighting feature
	uint     light_count;
	uint     shadows;       // cascade table, zero when no shadow feature runs
	uint     object_params; // per object float4 table, zero when absent
	uint     scene_depth;   // texture, valid in transparent and screen passes
	uint     scene_color;
	uint     _pad0;
	uint     _pad1;
};

EMBER_CONSTANTS(0, FrameConstants, g_frame)
EMBER_CONSTANTS(1, ViewConstants,  g_view)

// ---- Stage data ------------------------------------------------------------------------------
/// The vertex after the object transform, before projection. vertex() may edit it.
struct VertexData
{
	float3 position; // world
	float3 normal;   // world
	float3 color;
	float2 uv;
	float4 custom0;  // author owned: written by vertex(), read by surface()
	float4 object;   // per object params (zero without a table)
};

/// What surface() receives: the interpolated VertexData plus what the pass knows.
struct SurfaceInput
{
	float3 position;
	float3 normal;   // world, flipped to face the camera for two sided types
	float3 view_dir; // surface to camera
	float3 color;
	float2 uv;
	float4 custom0;
	float4 object;
	float2 screen_uv;
	bool   front_face;
};

static const uint SHADING_LIT   = 0;
static const uint SHADING_UNLIT = 1;

/// What surface() produces. Preset by the entry: normal from the vertex, opaque, lit, rough.
struct Surface
{
	float3 albedo;
	float3 normal;
	float  roughness;
	float  metallic;
	float  occlusion;
	float3 emissive;
	float  opacity;
	uint   shading;
};

interface IMaterial
{
	void surface(SurfaceInput s, inout Surface out);
	void vertex(inout VertexData v) {}
};

// ---- Helpers ---------------------------------------------------------------------------------
float3 unpack_normal(float4 texel) { return texel.xyz * 2.0 - 1.0; }
