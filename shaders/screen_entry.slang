EMBER_BUFFER_ALIAS(Material, ember_materials)

struct PushConstants
{
	uint materials;
	uint material; // the record: screen materials are drawn once, not per object
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
	VertexOutput output;
	output.uv       = float2((vertex_id << 1) & 2, vertex_id & 2);
	output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return output;
}

[shader("fragment")]
float4 fs_main(VertexOutput input) : SV_Target
{
	const Material material = ember_materials[g_push.materials][g_push.material];
	ScreenInput s;
	s.uv    = input.uv;
	s.pixel = input.position.xy;
	return material.pixel(s);
}
