#include "ember.slang"

// The render module's GPU contract: mirrors of render/scene.h and
// render/geometry.h, the table aliases, and the attribute decode helpers,
// shared by the engine cull kernel and every shader family that draws from the
// scene. One mirror, so a layout change breaks one file instead of every
// consumer.

struct ObjectData
{
	float4 sphere; // world space center and radius
	uint   geometry;
	uint   material;
	uint   flags;
	uint   layers; // zero marks a dead slot
};

struct TransformData
{
	float4 rows[3]; // world 3x4, row major
};

struct GeometryData
{
	uint   first_index;
	uint   index_count; // zero marks a destroyed geometry
	uint   first_vertex;
	uint   vertex_count;
	float4 sphere;
};

// Per-vertex attribute stream beside the float4 positions.
struct AttributeData
{
	uint   normal; // octahedral snorm16 pair
	uint   color;  // rgba8
	float2 uv;
};

// Bit identical to gpu::DrawIndexedIndirectArgs; scalars pack tight in std430,
// so the stride is the API's 20 bytes.
struct DrawArgs
{
	uint index_count;
	uint instance_count;
	uint first_index;
	int  base_vertex;
	uint first_instance;
};

EMBER_BUFFER_ALIAS(float4, ember_position_buffers)
EMBER_BUFFER_ALIAS(AttributeData, ember_attribute_buffers)
EMBER_BUFFER_ALIAS(ObjectData, ember_object_buffers)
EMBER_BUFFER_ALIAS(TransformData, ember_transform_buffers)
EMBER_BUFFER_ALIAS(GeometryData, ember_geometry_buffers)
EMBER_BUFFER_RW_ALIAS(DrawArgs, ember_draw_buffers)
EMBER_BUFFER_RW_ALIAS(uint, ember_count_buffers)

// Matches math/packing.h: low snorm16 is x, high is y.
float3 decode_octahedral(uint packed)
{
	const float2 e = float2(
		(float)(int)(packed << 16) / 2147483648.0,
		(float)(int)(packed & 0xFFFF0000u) / 2147483648.0);

	float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));

	if (n.z < 0.0)
		n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);

	return normalize(n);
}

float4 unpack_rgba8(uint packed)
{
	return float4(
		(packed >> 0) & 0xFF,
		(packed >> 8) & 0xFF,
		(packed >> 16) & 0xFF,
		(packed >> 24) & 0xFF) / 255.0;
}

/// World position through a row major 3x4.
float3 transform_point(TransformData transform, float3 position)
{
	const float4 point = float4(position, 1.0);

	return float3(
		dot(transform.rows[0], point),
		dot(transform.rows[1], point),
		dot(transform.rows[2], point));
}

/// Directions rotate through the upper 3x3. Non-uniform scale would need the
/// inverse transpose; families that allow it own that cost.
float3 transform_direction(TransformData transform, float3 direction)
{
	return float3(
		dot(transform.rows[0].xyz, direction),
		dot(transform.rows[1].xyz, direction),
		dot(transform.rows[2].xyz, direction));
}
