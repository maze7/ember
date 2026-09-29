#language slang 2026

// Shading models: how light meets a Surface. A material type names one with [Shading("name")] and
// the compiler links that struct in as the entry points' `Shading`, matching the name against the
// structs below that implement IShadingModel, ignoring case. Adding a model is adding a struct.
//
// Materials never see a light. The engine resolves the view's lights into a Lighting value and the
// model decides what to do with it, so light types, clustering and shadows live in one place.
module shading;

import ember;
import frame;
import material;

/// Mirror of the lighting feature's light record.
public struct LightData
{
	float4 position_range;  // xyz world position; w range, zero for a directional light
	float4 direction_angle; // xyz the direction the light travels; w cosine of the spot cone, -1 for none
	float4 color_intensity; // rgb linear colour, a intensity
};

[[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<LightData> g_lights[];

/// One light as it arrives at a point: the unit direction toward it, the radiance reaching the
/// point (colour, intensity and falloff folded in) and the fraction the shadow lets through.
public struct LightSample
{
	float3 direction;
	float3 radiance;
	float  shadow;
};

/// The view's lights as seen from one pixel. The shading model owns the loop over them, so a model
/// that ignores light costs nothing and one that quantises can do so after the sum.
public struct Lighting
{
	float3 ambient;
	uint   count;
	internal uint table;

	LightSample sample(uint index, float3 position)
	{
		const LightData light = g_lights[table][index];

		LightSample result;
		result.radiance = light.color_intensity.rgb * light.color_intensity.a;
		result.shadow   = 1.0; // shadow maps arrive with the shadow feature

		if (light.position_range.w == 0.0)
		{
			result.direction = -light.direction_angle.xyz;
			return result;
		}

		const float3 to_light = light.position_range.xyz - position;
		const float  distance = length(to_light);
		result.direction      = to_light / max(distance, 1e-4);

		// Inverse square, windowed to reach exactly zero at the range so a light's influence has a
		// hard edge the clustering can trust (Karis, Real Shading in Unreal Engine 4).
		const float window = saturate(1.0 - pow(distance / light.position_range.w, 4.0));
		const float cone   = light.direction_angle.w > -1.0
								 ? smoothstep(light.direction_angle.w, lerp(light.direction_angle.w, 1.0, 0.1),
											  dot(-result.direction, light.direction_angle.xyz))
								 : 1.0;

		result.radiance *= window * window * cone / (distance * distance + 1.0);
		return result;
	}
};

/// The lighting of the view the current pass renders.
public Lighting view_lighting()
{
	Lighting lighting;
	lighting.ambient = g_view.ambient;
	lighting.count   = g_view.light_count;
	lighting.table   = g_view.lights;
	return lighting;
}

public interface IShadingModel
{
	/// The colour leaving the surface toward the eye. Emissive is the model's to add, so a model
	/// may tone it too.
	static float3 shade(Surface s, SurfaceInput i, Lighting lighting);
};

/// The surface as authored: albedo plus emissive, no light. UI in the world, effects, debug views.
public struct Unlit : IShadingModel
{
	static float3 shade(Surface s, SurfaceInput i, Lighting lighting) { return s.albedo + s.emissive; }
};

/// Lambert under every light plus flat ambient: the placeholder until the Pixel and Standard models
/// arrive with the lighting feature. Light intensities fold in Lambert's 1/pi, as artist-facing
/// light units usually do, so a white light of intensity 1 lights a white surface to white.
public struct Lit : IShadingModel
{
	static float3 shade(Surface s, SurfaceInput i, Lighting lighting)
	{
		float3 incoming = lighting.ambient * s.occlusion;

		for (uint n = 0; n < lighting.count; ++n)
		{
			const LightSample light = lighting.sample(n, s.shading_position);
			incoming += light.radiance * (saturate(dot(s.normal, light.direction)) * light.shadow);
		}

		return s.albedo * incoming + s.emissive;
	}
};
