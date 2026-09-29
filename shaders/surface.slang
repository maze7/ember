#language slang 2026

// The surface domain's entry points: every pass that draws scene geometry with a material.
//
// The material type and its shading model are link-time types. The compiler loads the author's
// file, writes a two-line module exporting its struct as Material and the model it asked for as
// Shading, and links that against this module. Nothing in the engine names a material, and one
// checked copy of this module serves every type.
module surface;

import ember;
import render;
import frame;
import material;
import shading;

extern struct Material : IMaterial;
extern struct Shading : IShadingModel = Lit;

/// The only per-draw value: the drawn type's record table. A draw is one bucket and a bucket is one
/// type, so everything else is per frame or per view.
struct DrawConstants
{
	uint materials;
};

[[vk::push_constant]] ConstantBuffer<DrawConstants> g_draw;

[[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<Material> g_materials[];

/// A material key table row holds the draw bucket in its high half and the record's slot in the
/// type's table in its low half. Objects store the handle, so a material can change type or move
/// slot by rewriting one row.
uint material_slot(uint handle_index)
{
	return g_uints[g_frame.material_keys][handle_index] & 0xFFFFu;
}

struct Interpolants
{
	float4 position : SV_Position;
	float3 world    : WORLD;
	float3 normal   : NORMAL;
	float4 color    : COLOR;
	float2 uv       : TEXCOORD0;
	float4 custom   : TEXCOORD1;
	nointerpolation uint object : OBJECT;
	nointerpolation uint slot   : SLOT;
};

[shader("vertex")]
Interpolants vs_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID,
					 uint base_instance : SV_StartInstanceLocation)
{
	// The cull wrote the object's slot into first_instance. Slang lowers system values with D3D
	// semantics, so it arrives through the start location rather than inside SV_InstanceID.
	const uint object = base_instance + instance_id;

	const ObjectData    data      = g_objects[g_frame.objects][object];
	const TransformData transform = g_transforms[g_frame.transforms][object];
	const AttributeData attribute = g_attributes[g_frame.attributes][vertex_id];
	const uint          slot      = material_slot(data.material);

	VertexData v;
	v.position = transform_point(transform, g_positions[g_frame.positions][vertex_id].xyz);
	v.normal   = normalize(transform_direction(transform, decode_octahedral(attribute.normal)));
	v.color    = unpack_rgba8(attribute.color);
	v.uv       = attribute.uv;
	v.custom   = 0.0;
	v.origin   = transform_origin(transform);
	v.object   = object;
	v.time     = g_frame.time;

	// A type that keeps the default vertex() compiles this to nothing, record load included.
	g_materials[g_draw.materials][slot].vertex(v);

	Interpolants out;
	out.position = mul(g_view.view_projection, float4(v.position, 1.0));
	out.world    = v.position;
	out.normal   = v.normal;
	out.color    = v.color;
	out.uv       = v.uv;
	out.custom   = v.custom;
	out.object   = object;
	out.slot     = slot;
	return out;
}

SurfaceInput surface_input(Interpolants i, bool front_face)
{
	SurfaceInput s;
	s.position   = i.world;
	s.normal     = normalize(front_face ? i.normal : -i.normal);
	s.view_dir   = normalize(g_view.eye.xyz - i.world * g_view.eye.w);
	s.color      = i.color;
	s.uv         = i.uv;
	s.custom     = i.custom;
	s.screen_uv  = i.position.xy * g_view.inverse_resolution;
	s.pixel      = i.position.xy;
	s.front_face = front_face;
	s.object     = i.object;
	s.time       = g_frame.time;

	// Meshes carry no tangents yet, so the frame comes from screen derivatives of position and uv
	// (Schüler's cotangent frame). It is built here, before surface() can discard, and the compiler
	// drops it for every material that never reads it.
	const float3 dp_dx      = ddx(i.world);
	const float3 dp_dy      = ddy(i.world);
	const float2 duv_dx     = ddx(i.uv);
	const float2 duv_dy     = ddy(i.uv);
	const float3 dp_dy_perp = cross(dp_dy, s.normal);
	const float3 dp_dx_perp = cross(s.normal, dp_dx);
	const float3 tangent    = dp_dy_perp * duv_dx.x + dp_dx_perp * duv_dy.x;
	const float3 bitangent  = dp_dy_perp * duv_dx.y + dp_dx_perp * duv_dy.y;
	const float  scale      = rsqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-20));

	s.tangent   = tangent * scale;
	s.bitangent = bitangent * scale;
	return s;
}

Surface preset(SurfaceInput s)
{
	Surface surface;
	surface.albedo           = 1.0;
	surface.normal           = s.normal;
	surface.roughness        = 1.0;
	surface.metallic         = 0.0;
	surface.occlusion        = 1.0;
	surface.emissive         = 0.0;
	surface.opacity          = 1.0;
	surface.shading_position = s.position;
	return surface;
}

/// Colour passes: forward shading, opaque, cutout and transparent alike.
[shader("fragment")]
float4 fs_main(Interpolants i, bool front_face : SV_IsFrontFace) : SV_Target
{
	const SurfaceInput s = surface_input(i, front_face);

	Surface surface = preset(s);
	g_materials[g_draw.materials][i.slot].surface(s, surface);

	return float4(Shading.shade(surface, s, view_lighting()), surface.opacity);
}

/// Depth-only passes of cutout types: the prepass and shadow maps. surface() runs for its discard;
/// the colour is never written, so the compiler removes everything that only fed it. Opaque types
/// need no fragment stage at all in these passes.
[shader("fragment")]
void fs_depth(Interpolants i, bool front_face : SV_IsFrontFace)
{
	const SurfaceInput s = surface_input(i, front_face);

	Surface surface = preset(s);
	g_materials[g_draw.materials][i.slot].surface(s, surface);
}
