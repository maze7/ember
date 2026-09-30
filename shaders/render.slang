#language slang 2026

// The scene as the GPU sees it: mirrors of render/scene.h and render/geometry.h, byte for byte, and
// the typed views of the tables every scene pass indexes. One mirror, so a layout change breaks one
// file instead of every consumer. StructuredBuffer elements stride at std430 rules, which is why
// every struct here keeps its size a multiple of 16 bytes on both sides.
module render;

import ember;

/// Bytes of per-object data a material type may declare as `struct Instance`. Equals
/// material::INSTANCE_BYTES: the instance table holds one such record per scene slot.
public static const uint INSTANCE_STRIDE = 32;

/// Mirror of render::ObjectFlags.
public static const uint OBJECT_FLAG_CASTS_SHADOW = 1u << 0;
public static const uint OBJECT_FLAG_BILLBOARD    = 1u << 1;

public struct ObjectData
{
	float4 sphere;   // world space centre and radius
	uint   geometry; // slot in the geometry table
	uint   material; // the material handle's index
	uint   flags;    // OBJECT_FLAG_*
	uint   layers;   // layer mask; zero marks a dead slot
};

/// The world matrix's upper 3x4, row major, so a position transforms with three dot products.
public struct TransformData
{
	float4 rows[3];
};

public struct GeometryData
{
	uint   first_index;
	uint   index_count; // zero marks a destroyed geometry
	uint   first_vertex;
	uint   vertex_count;
	float4 sphere;      // authoring space centre and radius
};

/// The per-vertex stream beside the float4 positions.
public struct AttributeData
{
	uint   normal; // octahedral, two snorm16
	uint   color;  // rgba8
	float2 uv;
};

/// Bit identical to gpu::DrawIndexedIndirectArgs. Scalars pack tight in std430, so the stride is
/// the API's 20 bytes.
public struct DrawArgs
{
	uint index_count;
	uint instance_count;
	uint first_index;
	int  base_vertex;
	uint first_instance;
};

// Typed views of the storage buffer heap. The index into each array is a table's bindless slot,
// always uniform (it comes from constants), so these are indexed directly.
public [[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<float4>        g_positions[];
public [[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<AttributeData> g_attributes[];
public [[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<ObjectData>    g_objects[];
public [[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<TransformData> g_transforms[];
public [[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<GeometryData>  g_geometries[];
public [[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<uint>          g_uints[];

// Writable views for compute producers (the cull kernel's draw streams). Vulkan binds read-only and
// writable storage buffers through the same descriptor type, so the slot contract is unchanged.
public [[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] RWStructuredBuffer<DrawArgs> g_draw_args[];
public [[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] RWStructuredBuffer<uint>     g_counts[];

/// Matches math/packing.h: the low snorm16 is x, the high one y.
public float3 decode_octahedral(uint packed)
{
	const float2 e = float2(
		(float)(int)(packed << 16) / 2147483648.0,
		(float)(int)(packed & 0xFFFF0000u) / 2147483648.0);

	float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));

	if (n.z < 0.0)
		n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);

	return normalize(n);
}

public float4 unpack_rgba8(uint packed)
{
	return float4(
		(packed >> 0) & 0xFF,
		(packed >> 8) & 0xFF,
		(packed >> 16) & 0xFF,
		(packed >> 24) & 0xFF) / 255.0;
}

public float3 transform_point(TransformData transform, float3 position)
{
	const float4 point = float4(position, 1.0);

	return float3(
		dot(transform.rows[0], point),
		dot(transform.rows[1], point),
		dot(transform.rows[2], point));
}

/// Directions rotate through the upper 3x3, which is exact for rotation and uniform scale. A
/// non-uniformly scaled object would need the inverse transpose, a cost only such objects should pay.
public float3 transform_direction(TransformData transform, float3 direction)
{
	return float3(
		dot(transform.rows[0].xyz, direction),
		dot(transform.rows[1].xyz, direction),
		dot(transform.rows[2].xyz, direction));
}

/// The object's origin in world space: the translation column.
public float3 transform_origin(TransformData transform)
{
	return float3(transform.rows[0].w, transform.rows[1].w, transform.rows[2].w);
}
