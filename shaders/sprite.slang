#include "render.slang"

// Mirror of render/features/sprite.h.
struct SpriteMaterial
{
	uint   albedo;
	uint   albedo_sampler;
	float  alpha_cutoff;
	uint   pad;
	float2 uv_min;
	float2 uv_max;
	float4 tint;
};

EMBER_BUFFER_ALIAS(SpriteMaterial, ember_sprite_materials)

struct SpriteConstants
{
	float4x4 view_proj;
};

EMBER_CONSTANTS(0, SpriteConstants, g_frame)

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
	float2 uv       : TEXCOORD0;
	nointerpolation uint material : TEXCOORD1;
};

[shader("vertex")]
VertexOutput vs_main(
	uint vertex_id : SV_VertexID,
	uint instance_id : SV_InstanceID,
	uint base_instance : SV_StartInstanceLocation)
{
	// Same object identity path as the mesh family: the cull wrote the slot into
	// first_instance, and the start location builtin carries it back.
	const uint slot = base_instance + instance_id;

	const TransformData transform = ember_transform_buffers[g_push.transforms][slot];
	const float3 local			  = ember_position_buffers[g_push.positions][vertex_id].xyz;
	const AttributeData attribute = ember_attribute_buffers[g_push.attributes][vertex_id];
	const uint material_id		  = ember_object_buffers[g_push.objects][slot].material;

	const SpriteMaterial material = ember_sprite_materials[g_push.materials][material_id];

	VertexOutput output;
	output.position = mul(g_frame.view_proj, float4(transform_point(transform, local), 1.0));
	output.normal   = normalize(transform_direction(transform, decode_octahedral(attribute.normal)));

	// Baked standees carry final atlas uvs and an identity rect; unit quads carry
	// 0..1 corners and a per frame rect. One lerp serves both.
	output.uv       = lerp(material.uv_min, material.uv_max, attribute.uv);
	output.material = material_id;
	return output;
}

[shader("fragment")]
float4 fs_main(VertexOutput input) : SV_Target
{
	const SpriteMaterial material = ember_sprite_materials[g_push.materials][input.material];

	// Material indices vary per draw inside one indirect batch and waves pack
	// across draws, so the descriptor indices are not uniform here.
	const float4 texel = EMBER_TEX2D_NU(material.albedo)
							 .SampleLevel(EMBER_SAMPLER_NU(material.albedo_sampler), input.uv, 0.0);

	if (texel.a < material.alpha_cutoff)
		discard;

	// The same fixed top light as the mesh family so standees sit in the world
	// instead of floating flat; the lit families replace this uniformly later.
	const float light = 0.6 + 0.4 * saturate(input.normal.y * 0.7 + input.normal.z * 0.5 + 0.3);

	return float4(texel.rgb * material.tint.rgb * light, 1.0);
}
