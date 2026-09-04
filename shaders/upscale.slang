#include "ember.slang"

struct UpscaleConstants
{
	float2 scene_size;
	float2 scale; // output pixels per scene texel, per axis
	float2 subtexel_offset;
	float2 pad;
};

EMBER_CONSTANTS(1, UpscaleConstants, g_upscale)

struct PushConstants
{
	uint scene_color;
	uint scene_sampler;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;

struct VertexOutput
{
	float4 position : SV_Position;
	float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vs_main(uint vertex_id : SV_VertexID)
{
	// One triangle covering the screen; both passes share the engine's flipped
	// viewport convention, so the round trip through scene_color is identity.
	const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);

	VertexOutput output;
	output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
	output.uv       = uv;
	return output;
}

[shader("fragment")]
float4 fs_main(VertexOutput input) : SV_Target
{
	// Sharp bilinear: nearest inside the texel, one output pixel of linear
	// blend at texel edges. The snapped camera's remainder shifts the sampling
	// window so panning stays smooth while geometry stays pixel locked.
	const float2 p = input.uv * g_upscale.scene_size + g_upscale.subtexel_offset;

	const float2 base  = floor(p);
	const float2 sharp = clamp((p - base - 0.5) * g_upscale.scale, -0.5, 0.5) + 0.5;

	const float2 uv = (base + sharp) / g_upscale.scene_size;

	return EMBER_TEX2D(g_push.scene_color).SampleLevel(EMBER_SAMPLER(g_push.scene_sampler), uv, 0.0);
}
