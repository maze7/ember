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
	// One triangle covering the screen. The flipped viewport puts ndc +y at
	// the top of the screen while row zero of scene_color holds the scene's
	// top, so v must run opposite to ndc y or this pass inverts the image.
	const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);

	VertexOutput output;
	output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
	output.uv       = float2(uv.x, 1.0 - uv.y);
	return output;
}

[shader("fragment")]
float4 fs_main(VertexOutput input) : SV_Target
{
	// Sharp bilinear: nearest inside a texel, one output pixel of blend at
	// texel edges, pure nearest at integer scales. The clamp region is the
	// span of a texel that maps inside one output pixel; only the remainder
	// blends. The snapped camera's remainder shifts the sampling window so
	// panning stays smooth while geometry stays pixel locked.
	const float2 offset = float2(-g_upscale.subtexel_offset.x, g_upscale.subtexel_offset.y);
	const float2 p      = input.uv * g_upscale.scene_size + offset;

	const float2 base  = floor(p);
	const float2 f     = p - base - 0.5;
	const float2 range = max(0.5 - 0.5 / g_upscale.scale, 0.0);
	const float2 sharp = (f - clamp(f, -range, range)) * g_upscale.scale + 0.5;

	const float2 uv = (base + sharp) / g_upscale.scene_size;

	return EMBER_TEX2D(g_push.scene_color).SampleLevel(EMBER_SAMPLER(g_push.scene_sampler), uv, 0.0);
}
