#include "ember.slang"

// ImDrawVert is 20 packed bytes. A StructuredBuffer of {float2, float2, uint}
// strides at 24 under std430, so the vertex loads manually from the byte buffer.
static const uint VERTEX_STRIDE = 20;

struct Push
{
	float2 scale;
	float2 translate;
	uint   vertex_buffer;
	uint   texture;
	uint   sampler;
	uint   first_vertex;
};

[[vk::push_constant]] ConstantBuffer<Push> g_push;

struct VertexOutput
{
	float4 position : SV_Position;
	float4 color    : COLOR0;
	float2 uv       : TEXCOORD0;
};

// ImGui colors are sRGB bytes. The swapchain is an sRGB format, so blending runs
// in linear space and the ROP re-encodes on write. pow 2.2 is close enough for UI.
float4 decode_color(uint packed)
{
	float4 color = float4(
		(packed >> 0) & 0xFF,
		(packed >> 8) & 0xFF,
		(packed >> 16) & 0xFF,
		(packed >> 24) & 0xFF) / 255.0;

	return float4(pow(color.rgb, 2.2), color.a);
}

[shader("vertex")]
VertexOutput vs_main(uint vertex_id : SV_VertexID)
{
	// SV_VertexID excludes the draw's base vertex on every Slang target, so the
	// ring element base arrives through the push block.
	const uint offset = (vertex_id + g_push.first_vertex) * VERTEX_STRIDE;

	const float2 position = asfloat(EMBER_BUFFER(g_push.vertex_buffer).Load2(offset + 0));
	const float2 uv       = asfloat(EMBER_BUFFER(g_push.vertex_buffer).Load2(offset + 8));
	const uint   color    = EMBER_BUFFER(g_push.vertex_buffer).Load(offset + 16);

	VertexOutput output;
	output.position = float4(position * g_push.scale + g_push.translate, 0.0, 1.0);
	output.color    = decode_color(color);
	output.uv       = uv;
	return output;
}

[shader("fragment")]
float4 fs_main(VertexOutput input) : SV_Target
{
	return input.color * EMBER_TEX2D(g_push.texture).Sample(EMBER_SAMPLER(g_push.sampler), input.uv);
}
