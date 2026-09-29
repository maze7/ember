#language slang 2026

// The material authoring API: everything a material file may name.
//
// A material type is one file that imports this module and declares one struct implementing
// IMaterial (drawn on scene geometry) or IScreenMaterial (drawn over the whole screen). The
// interface it implements is its domain, its fields are its parameters, and its attributes are its
// pipeline state. The engine supplies everything else: entry points, vertex fetch, passes, culling
// and lighting.
//
// The engine modules are imported below without being re-exported, so nothing internal is in scope
// in a material file: naming a binding, the heap or a scene table is an ordinary "undefined
// identifier". A material file that imports an engine module itself would bring them back, so the
// material compiler refuses that too; the boundary holds against intent as well as accident.
module material;

import ember;
import render;
import frame;

// Read by reflection when the type compiles; free at runtime. An unknown value is a compile error
// that lists the accepted ones.

/// Which pass family draws the type: "opaque" (the default), "cutout" (alpha tested: depth writing
/// and order independent) or "transparent" (blended after the opaque world).
[__AttributeUsage(_AttributeTargets.Struct)] public struct QueueAttribute { string name; };

/// "back" (the default), "none" or "front".
[__AttributeUsage(_AttributeTargets.Struct)] public struct CullAttribute { string mode; };

/// Transparent types only: "alpha" (the default there), "premultiplied" or "additive".
[__AttributeUsage(_AttributeTargets.Struct)] public struct BlendAttribute { string mode; };

/// Transparent types default to false, everything else to true.
[__AttributeUsage(_AttributeTargets.Struct)] public struct DepthWriteAttribute { bool enabled; };

/// Whether the type draws into shadow maps; true by default. Objects can still opt out one by one.
[__AttributeUsage(_AttributeTargets.Struct)] public struct ShadowAttribute { bool casts; };

/// Draw order inside the queue, lower first. Matters for transparent types, which do not sort.
[__AttributeUsage(_AttributeTargets.Struct)] public struct PriorityAttribute { int order; };

/// The shading model, by name: a struct in shading.slang. Absent means the project's default.
[__AttributeUsage(_AttributeTargets.Struct)] public struct ShadingAttribute { string model; };

/// The inspector's label; the field name when absent.
[__AttributeUsage(_AttributeTargets.Var)] public struct DisplayAttribute { string name; };

/// A slider between min and max. Values in files are not clamped, so a script can overdrive them.
[__AttributeUsage(_AttributeTargets.Var)] public struct RangeAttribute { float min; float max; };

/// A float3 or float4 edited with a colour picker. Files hold sRGB, which is what an artist's
/// picker means, and the encoder converts to linear.
[__AttributeUsage(_AttributeTargets.Var)] public struct ColorAttribute {};

/// With [Color]: linear and unbounded, for emissive intensity. Files hold linear values.
[__AttributeUsage(_AttributeTargets.Var)] public struct HdrAttribute {};

/// The value an instance gets when its file does not set one, written the way files write it:
/// "0.5", "1 0.8 0.6 1", or for textures one of "white", "black", "flat" (a normal map facing out)
/// or "error". Without it, numbers are zero and textures white.
[__AttributeUsage(_AttributeTargets.Var)] public struct DefaultAttribute { string value; };

/// Textures: "point" or "linear", the default sampler filter an instance may override.
[__AttributeUsage(_AttributeTargets.Var)] public struct FilterAttribute { string mode; };

/// Textures: "repeat", "mirror", "clamp" or "border".
[__AttributeUsage(_AttributeTargets.Var)] public struct WrapAttribute { string mode; };

/// Textures: the texels are data (normals, masks), not colour. The encoder warns when an
/// sRGB texture is bound here, because the hardware would decode it and bend every value.
[__AttributeUsage(_AttributeTargets.Var)] public struct LinearAttribute {};

// A bindless texture and sampler pair: 8 bytes in the record, written by the engine. Sampling uses
// the divergent accessors because records vary per draw inside one multi-draw and a wave can hold
// pixels from several draws; on a uniform index that costs one pass of a scalarisation loop.

public struct Texture2DRef
{
	internal uint texture;
	internal uint sampler;

	/// Implicit level of detail, from screen derivatives: fragment stages only.
	float4 sample(float2 uv) { return texture_2d_nu(texture).Sample(sampler_nu(sampler), uv); }
	float4 sample_level(float2 uv, float level) { return texture_2d_nu(texture).SampleLevel(sampler_nu(sampler), uv, level); }
	float4 sample_bias(float2 uv, float bias) { return texture_2d_nu(texture).SampleBias(sampler_nu(sampler), uv, bias); }
	float4 sample_grad(float2 uv, float2 dx, float2 dy) { return texture_2d_nu(texture).SampleGrad(sampler_nu(sampler), uv, dx, dy); }

	/// One texel, unfiltered: pixel art that must not blend, lookup tables.
	float4 load(int2 texel, int level = 0) { return texture_2d_nu(texture).Load(int3(texel, level)); }

	/// Texels across the given level.
	float2 size(uint level = 0)
	{
		uint width, height, levels;
		texture_2d_nu(texture).GetDimensions(level, width, height, levels);
		return float2(width, height);
	}
};

public struct Texture2DArrayRef
{
	internal uint texture;
	internal uint sampler;

	float4 sample(float2 uv, float layer) { return texture_2d_array_nu(texture).Sample(sampler_nu(sampler), float3(uv, layer)); }
	float4 sample_level(float2 uv, float layer, float level) { return texture_2d_array_nu(texture).SampleLevel(sampler_nu(sampler), float3(uv, layer), level); }
	float4 load(int2 texel, int layer, int level = 0) { return texture_2d_array_nu(texture).Load(int4(texel, layer, level)); }
};

public struct TextureCubeRef
{
	internal uint texture;
	internal uint sampler;

	float4 sample(float3 direction) { return texture_cube_nu(texture).Sample(sampler_nu(sampler), direction); }
	float4 sample_level(float3 direction, float level) { return texture_cube_nu(texture).SampleLevel(sampler_nu(sampler), direction, level); }
};

/// A vertex in world space, after the object's transform and before projection. vertex() may move
/// it. The fields after `custom` are context: the engine ignores changes to them.
public struct VertexData
{
	float3 position; // world
	float3 normal;   // world, unit length
	float4 color;    // vertex colour, as stored
	float2 uv;
	float4 custom;   // author owned: written here, interpolated into SurfaceInput.custom

	float3 origin;   // the object's world origin: billboard pivots, per-object phase
	uint   object;   // the object's scene slot; instance<T>() reads through it
	float  time;     // FrameConstants.time
};

/// What surface() receives: VertexData interpolated, plus what only the pixel knows.
public struct SurfaceInput
{
	float3 position;  // world
	float3 normal;    // world, unit length, facing the viewer on back faces
	float3 tangent;   // world, along +u; from screen derivatives until meshes carry tangents
	float3 bitangent; // world, along +v
	float3 view_dir;  // unit, from the surface toward the eye
	float4 color;
	float2 uv;
	float4 custom;

	float2 screen_uv; // 0..1 across the render target
	float2 pixel;     // the render target pixel's centre
	bool   front_face;
	uint   object;
	float  time;

	/// A tangent-space direction, such as a normal map sample, in world space.
	float3 to_world(float3 v) { return normalize(v.x * tangent + v.y * bitangent + v.z * normal); }
};

/// What surface() produces. It arrives preset to a white, fully rough, opaque surface facing along
/// the interpolated normal, so a material writes only what it changes.
public struct Surface
{
	float3 albedo;           // base colour, linear
	float3 normal;           // world, unit length
	float  roughness;
	float  metallic;
	float  occlusion;
	float3 emissive;         // linear, added after lighting
	float  opacity;          // blended by transparent types, ignored by the rest
	float3 shading_position; // where lights and shadows are evaluated: the pixel, or a texel centre
};

public struct ScreenInput
{
	float2 uv;    // 0..1 across the target, v running down
	float2 pixel; // the target pixel's centre
	float  time;
};

/// A material drawn on scene geometry: meshes, tiles, sprites.
public interface IMaterial
{
	/// Describes the surface at one pixel. `out` arrives preset (see Surface). May discard; the
	/// engine lowers discard to demote-to-helper, so quad neighbours keep valid derivatives and
	/// mip selection and snap_to_texels stay correct along cutout edges.
	void surface(SurfaceInput s, inout Surface out);

	/// Moves vertices: wind, billboards, squash. Runs in every pass that draws the type, depth and
	/// shadow passes included, so shadows move with the surface. Replace it with
	/// `override void vertex(inout VertexData v)`.
	void vertex(inout VertexData v) {}
};

/// A material drawn over the whole target once per frame: post effects, palette mapping,
/// outlines. Reads the scene through scene_color() and scene_depth().
public interface IScreenMaterial
{
	float4 pixel(ScreenInput s);
};

/// The per-object values a type declares as a nested `struct Instance`. The compiler holds that
/// struct to 32-bit scalars and vectors, 32 bytes at most, packed in declaration order, which is
/// exactly how this load reads it, so the C++ side writes plain packed bytes.
public T instance<T>(SurfaceInput s) { return byte_buffer(g_frame.instances).Load<T>(s.object * INSTANCE_STRIDE); }
public T instance<T>(VertexData v) { return byte_buffer(g_frame.instances).Load<T>(v.object * INSTANCE_STRIDE); }

/// Moves where lighting is evaluated to the centre of the texel `uv` falls in on `grid`, so light
/// and shadow edges land on the art's pixel grid instead of cutting through art pixels. It solves
/// for the screen step that moves uv to the texel centre and walks the world position by the same
/// step. Needs screen derivatives, so fragment stages only.
public void snap_to_texels(SurfaceInput s, Texture2DRef grid, float2 uv, inout Surface out)
{
	const float2 size  = grid.size();
	const float2 texel = uv * size;
	const float2 delta = (floor(texel) + 0.5 - texel) / size; // uv offset to the texel centre

	// [ddx(uv) ddy(uv)] * step = delta, solved with the 2x2 inverse.
	const float2 du  = ddx(uv);
	const float2 dv  = ddy(uv);
	const float  det = du.x * dv.y - dv.x * du.y;

	// uv that does not change across the pixel has no grid to snap to.
	if (abs(det) < 1e-12)
		return;

	const float2 step = float2(dv.y * delta.x - dv.x * delta.y, du.x * delta.y - du.y * delta.x) / det;

	out.shading_position = s.position + ddx(s.position) * step.x + ddy(s.position) * step.y;
}

/// An RGB normal map texel to a tangent-space direction.
public float3 unpack_normal(float4 texel) { return normalize(texel.xyz * 2.0 - 1.0); }

/// Rec. 709 luminance of a linear colour.
public float luminance(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

/// The opaque world's colour and depth at a screen position. Valid in transparent and screen
/// passes, which run after it; elsewhere they read the heap's fallback.
public float4 scene_color(float2 screen_uv) { return texture_2d(g_view.scene_color).SampleLevel(sampler(g_view.scene_sampler), screen_uv, 0.0); }
public float  scene_depth(float2 screen_uv) { return texture_2d(g_view.scene_depth).SampleLevel(sampler(g_view.scene_sampler), screen_uv, 0.0).r; }
