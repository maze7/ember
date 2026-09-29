import ember;
import render;

// Mirror of render/features/mesh.h.
struct MeshMaterial
{
	uint   albedo;
	uint   albedo_sampler;
	uint   mip;
	uint   pad;
	float4 tint;
};

[[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<MeshMaterial> g_mesh_materials[];

struct MeshConstants
{
	float4x4 view_proj;
};

[[vk::binding(CONSTANTS_FRAME, SET_CONSTANTS)]] ConstantBuffer<MeshConstants> g_mesh;

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

	const TransformData transform = g_transforms[g_push.transforms][slot];
	const float3 local = g_positions[g_push.positions][vertex_id].xyz;
	const AttributeData attribute = g_attributes[g_push.attributes][vertex_id];

	VertexOutput output;
	output.position = mul(g_mesh.view_proj, float4(transform_point(transform, local), 1.0));
	output.normal   = normalize(transform_direction(transform, decode_octahedral(attribute.normal)));
	output.color    = unpack_rgba8(attribute.color).rgb;
	output.uv       = attribute.uv;
	output.material = g_objects[g_push.objects][slot].material;
	return output;
}

[shader("fragment")]
float4 fs_main(VertexOutput input) : SV_Target
{
	const MeshMaterial material = g_mesh_materials[g_push.materials][input.material];

	float4 texel = texture_2d_nu(material.albedo).SampleLevel(sampler_nu(material.albedo_sampler), input.uv, (float)material.mip);

	// Cutout: tile and sprite art carries meaning in alpha. Opaque textures
	// sample a = 1 everywhere and never take this branch. Lives here until the
	// family split gives cutout its own pipeline.
	if (texel.a < 0.01)
		discard;

	// A fixed top light keyed off the decoded normal, so a broken attribute or
	// octahedral path shows up as wrong shading instead of hiding.
	const float light = 0.6 + 0.4 * saturate(input.normal.y * 0.7 + input.normal.z * 0.5 + 0.3);

	return float4(texel.rgb * material.tint.rgb * input.color * light, 1.0);
}
