import ember;
import render;
import frame;

/// Where one bucket's draws go in a view's argument buffer. Mirror of render::BucketRange.
struct BucketRange
{
	uint first;
	uint capacity;
};

[[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<BucketRange> g_bucket_ranges[];

/// Mirror of the cull constants in render/visibility.cpp. The scene's tables come from the frame's
/// constants, like every scene pass's; only what differs per view is here.
struct CullConstants
{
	float4 planes[6];
	uint   slot_count;
	uint   layers;
	uint   buckets;      // the frame's bucket ranges, in the transient ring
	uint   first_bucket; // the element of bucket 0's range there
};

[[vk::binding(CONSTANTS_PASS, SET_CONSTANTS)]] ConstantBuffer<CullConstants> g_cull;

struct PushConstants
{
	uint args;   // DrawArgs: each bucket's draws from its range's first entry
	uint counts; // one uint per bucket
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_push;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 thread : SV_DispatchThreadID)
{
	const uint slot = thread.x;
	if (slot >= g_cull.slot_count)
		return;

	const ObjectData object = g_objects[g_frame.objects][slot];

	// Dead slots carry layer mask zero, so one test retires them and applies the view's layer
	// selection together.
	if ((object.layers & g_cull.layers) == 0)
		return;

	// Mirror of render::intersects, the CPU reference. The infinite far plane has a zero normal and
	// positive w, which every sphere passes.
	[unroll]
	for (int i = 0; i < 6; ++i)
	{
		const float4 plane = g_cull.planes[i];
		if (dot(plane.xyz, object.sphere.xyz) + plane.w < -object.sphere.w)
			return;
	}

	const GeometryData geometry = g_geometries[g_frame.geometries][object.geometry];
	if (geometry.index_count == 0)
		return;

	// The object names its material; the material's key names the bucket its type draws in.
	const uint        bucket = g_uints[g_frame.material_keys][object.material] >> 16;
	const BucketRange range  = g_bucket_ranges[g_cull.buckets][g_cull.first_bucket + bucket];

	uint index;
	InterlockedAdd(g_counts[g_push.counts][bucket], 1, index);

	// A range holds every object of its bucket's materials, counted this frame, so this is a guard
	// against a bookkeeping bug, never a path: past the range belongs to the next bucket.
	if (index >= range.capacity)
		return;

	DrawArgs args;
	args.index_count    = geometry.index_count;
	args.instance_count = 1;
	args.first_index    = geometry.first_index;
	args.base_vertex    = 0;
	args.first_instance = slot; // object identity; the vertex stage adds it back from the start location

	g_draw_args[g_push.args][range.first + index] = args;
}
