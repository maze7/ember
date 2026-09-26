// Appended behind every surface material by the cooker. Expects `struct Material : IMaterial`
// from the file in front of it, which included material.slang.
#include "lighting.slang"

EMBER_BUFFER_ALIAS(Material, ember_materials)
EMBER_BUFFER_ALIAS(float4, ember_param_buffers)

struct PushConstants
{
	uint positions;
	uint attributes;
	uint objects;
	uint transforms;
	uint materials; // this type's table
	uint _pad;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;

struct VertexOutput
{
	float4 position : SV_Position;
	float3 world    : TEXCOORD0;
	float3 normal   : NORMAL0;
	float3 color    : COLOR0;
	float2 uv       : TEXCOORD1;
	float4 custom0  : TEXCOORD2;
	float4 object   : TEXCOORD3;
	nointerpolation uint material : TEXCOORD4;
};

[shader("vertex")]
VertexOutput vs_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID, uint base_instance : SV_StartInstanceLocation)
{
	const uint slot = base_instance + instance_id;

	const ObjectData    object    = ember_object_buffers[g_push.objects][slot];
	const TransformData transform = ember_transform_buffers[g_push.transforms][slot];
	const AttributeData attribute = ember_attribute_buffers[g_push.attributes][vertex_id];

	const uint material_slot = object.material & 0xFFFFu;
	const Material material  = ember_materials[g_push.materials][material_slot];

	VertexData v;
	v.position = transform_point(transform, ember_position_buffers[g_push.positions][vertex_id].xyz);
	v.normal   = normalize(transform_direction(transform, decode_octahedral(attribute.normal)));
	v.color    = unpack_rgba8(attribute.color).rgb;
	v.uv       = attribute.uv;
	v.custom0  = 0;
	v.object   = g_view.object_params != 0 ? ember_param_buffers[g_view.object_params][slot] : float4(0, 0, 0, 0);

	material.vertex(v);

	VertexOutput output;
	output.position = mul(g_view.view_proj, float4(v.position, 1.0));
	output.world    = v.position;
	output.normal   = v.normal;
	output.color    = v.color;
	output.uv       = v.uv;
	output.custom0  = v.custom0;
	output.object   = v.object;
	output.material = material_slot;
	return output;
}

SurfaceInput surface_input(VertexOutput input, bool front_face)
{
	SurfaceInput s;
	s.position   = input.world;
	s.normal     = normalize(front_face ? input.normal : -input.normal);
	s.view_dir   = normalize(g_view.camera_position - input.world);
	s.color      = input.color;
	s.uv         = input.uv;
	s.custom0    = input.custom0;
	s.object     = input.object;
	s.screen_uv  = input.position.xy * g_view.inv_resolution;
	s.front_face = front_face;
	return s;
}

Surface default_surface(SurfaceInput s)
{
	Surface out;
	out.albedo    = 1.0;
	out.normal    = s.normal;
	out.roughness = 1.0;
	out.metallic  = 0.0;
	out.occlusion = 1.0;
	out.emissive  = 0.0;
	out.opacity   = 1.0;
	out.shading   = SHADING_LIT;
	return out;
}

[shader("fragment")]
float4 fs_main(VertexOutput input, bool front_face : SV_IsFrontFace) : SV_Target
{
	const Material material = ember_materials[g_push.materials][input.material];
	const SurfaceInput s    = surface_input(input, front_face);

	Surface surface = default_surface(s);
	material.surface(s, surface);

	return float4(shade(surface, s), surface.opacity);
}

/// Depth only (prepass, shadow maps): surface() runs for its discard and nothing is written.
[shader("fragment")]
void fs_depth(VertexOutput input, bool front_face : SV_IsFrontFace)
{
	const Material material = ember_materials[g_push.materials][input.material];
	const SurfaceInput s    = surface_input(input, front_face);
	Surface surface = default_surface(s);
	material.surface(s, surface);
}
