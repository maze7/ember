#include "render.slang"

struct CullConstants
{
	float4 planes[6];
	uint objects;
	uint geometries;
	uint object_count;
	uint layers;
};

EMBER_CONSTANTS(1, CullConstants, g_cull)

struct PushConstants
{
	uint args;
	uint count;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 thread : SV_DispatchThreadID)
{
	const uint slot = thread.x;
	if (slot >= g_cull.object_count)
		return;

	const ObjectData object = ember_object_buffers[g_cull.objects][slot];

	// Dead slots carry layer mask zero, so one test retires them and applies
	// the view's layer selection together.
	if ((object.layers & g_cull.layers) == 0)
		return;

	// Mirror of render::intersects, the CPU reference. The infinite far plane
	// has a zero normal and positive w, which every sphere passes.
	[unroll]
	for (int i = 0; i < 6; ++i)
	{
		const float4 plane = g_cull.planes[i];
		if (dot(plane.xyz, object.sphere.xyz) + plane.w < -object.sphere.w)
			return;
	}

	const GeometryData geometry = ember_geometry_buffers[g_cull.geometries][object.geometry];
	if (geometry.index_count == 0)
		return;

	uint draw_index;
	InterlockedAdd(ember_count_buffers[g_push.count][0], 1, draw_index);

	DrawArgs args;
	args.index_count    = geometry.index_count;
	args.instance_count = 1;
	args.first_index    = geometry.first_index;
	args.base_vertex    = 0;
	args.first_instance = slot; // object identity; the vertex stage adds it back from the start location

	ember_draw_buffers[g_push.args][draw_index] = args;
}
