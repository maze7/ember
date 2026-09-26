#pragma once
#include "material.slang"

// Engine shading. Materials never include this; the surface entry does.
struct LightData
{
	float4 position_range;   // xyz world, w range (0 = directional)
	float4 direction_angle;  // xyz direction, w spot cos
	float4 color_intensity;
};

EMBER_BUFFER_ALIAS(LightData, ember_light_buffers)

float3 shade(Surface s, SurfaceInput i)
{
	if (s.shading == SHADING_UNLIT)
		return s.emissive;

	float3 result = s.emissive + s.albedo * 0.05 * s.occlusion;

	for (uint l = 0; l < g_view.light_count; ++l)
	{
		const LightData light = ember_light_buffers[g_view.lights][l];
		float3 to_light = light.position_range.w == 0.0 ? -light.direction_angle.xyz : normalize(light.position_range.xyz - i.position);
		const float ndl = saturate(dot(s.normal, to_light));
		result += s.albedo * light.color_intensity.rgb * light.color_intensity.a * ndl;
	}

	return result;
}
