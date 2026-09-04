#include "render.slang"

// Mirror of render/features/mesh.h.
struct MeshMaterial
{
	uint   albedo;
	uint   albedo_sampler;
	uint   mip;
	uint   pad;
	float4 tint;
};

EMBER_BUFFER_ALIAS(MeshMaterial, ember_mesh_materials)

struct MeshConstants
{
	float4x4 view_proj;
};

EMBER_CONSTANTS(0, MeshConstants, g_frame)

struct PushConstants
{
	uint positions;
	uint attributes;
	uint objects;
	uint transforms;
	uint materials;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;

struct VertexOutput
{
	float4 position : SV_Position;
	float3 normal   : NORMAL0;
	float3 color    : COLOR0;
	float2 uv       : TEXCOORD0;
	nointerpolation uint material : TEXCOORD1;
};

[shader("vertex")]
VertexOutput vs_main(
	uint vertex_id : SV_VertexID,
	uint instance_id : SV_InstanceID,
	uint base_instance : SV_StartInstanceLocation)
{
	// Slang lowers system values with D3D semantics, so first_instance never
	// reaches SV_InstanceID on its own; the start location builtin carries it
	// (shaderDrawParameters, pinned at boot). The cull wrote the object slot
	// there, one instance per draw.
	const uint slot = base_instance + instance_id;

	const TransformData transform = ember_transform_buffers[g_push.transforms][slot];
	const float3 local			  = ember_position_buffers[g_push.positions][vertex_id].xyz;
	const AttributeData attribute = ember_attribute_buffers[g_push.attributes][vertex_id];

	VertexOutput output;
	output.position = mul(g_frame.view_proj, float4(transform_point(transform, local), 1.0));
	output.normal   = normalize(transform_direction(transform, decode_octahedral(attribute.normal)));
	output.color    = unpack_rgba8(attribute.color).rgb;
	output.uv       = attribute.uv;
	output.material = ember_object_buffers[g_push.objects][slot].material;
	return output;
}

[shader("fragment")]
float4 fs_main(VertexOutput input) : SV_Target
{
	const MeshMaterial material = ember_mesh_materials[g_push.materials][input.material];

	// Material indices vary per draw inside one indirect batch and waves can
	// pack across draws, so the descriptor indices are not uniform here.
	float4 texel = EMBER_TEX2D_NU(material.albedo)
					   .SampleLevel(EMBER_SAMPLER_NU(material.albedo_sampler), input.uv, (float)material.mip);

	// A fixed top light keyed off the decoded normal, so a broken attribute or
	// octahedral path shows up as wrong shading instead of hiding.
	const float light = 0.6 + 0.4 * saturate(input.normal.y * 0.7 + input.normal.z * 0.5 + 0.3);

	return float4(texel.rgb * material.tint.rgb * input.color * light, 1.0);
}
