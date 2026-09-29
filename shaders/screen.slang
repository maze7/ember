#language slang 2026

// The screen domain's entry points: a material drawn once over the whole target. Linked like the
// surface domain, with the author's struct exported as Material.
module screen;

import ember;
import frame;
import material;

extern struct Material : IScreenMaterial;

/// A screen material draws once, so its record comes straight from the push block.
struct DrawConstants
{
	uint materials;
	uint slot;
};

[[vk::push_constant]] ConstantBuffer<DrawConstants> g_draw;

[[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<Material> g_materials[];

struct Interpolants
{
	float4 position : SV_Position;
	float2 uv       : TEXCOORD0;
};

[shader("vertex")]
Interpolants vs_main(uint vertex_id : SV_VertexID)
{
	// One triangle covering the target. The viewport is flipped, so ndc +y is the top of the
	// screen, while row zero of every texture is its top: y flips on the way out and v runs down.
	const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);

	Interpolants out;
	out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	out.uv       = uv;
	return out;
}

[shader("fragment")]
float4 fs_main(Interpolants i) : SV_Target
{
	ScreenInput s;
	s.uv    = i.uv;
	s.pixel = i.position.xy;
	s.time  = g_frame.time;

	return g_materials[g_draw.materials][g_draw.slot].pixel(s);
}
