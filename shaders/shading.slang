#language slang 2026

// Shading models: how light meets a Surface. A material type names one with [Shading("name")] and
// the compiler links that struct in as the entry points' `Shading`, matching the name against the
// structs below that implement IShadingModel, ignoring case. Adding a model is adding a struct.
//
// Materials never see a light. The engine resolves the scene's lights into a Lighting value and the
// model decides what to do with it, so light types, clustering and shadows live in one place.
//
// Light units are artist units: a light's intensity is what it does to a white matte surface facing
// it, so a white light of intensity 1 lights white to white. That folds Lambert's 1/pi into every
// light, which is why the models below carry a pi where the textbooks carry none.
module shading;

import ember;
import frame;
import material;

/// Mirror of render::LightData: one light as the scene's light table holds it.
public struct LightData
{
	float3 position;    // world; unused by a directional light
	float  range;       // where the light's reach ends; zero marks a directional light
	float3 direction;   // unit, the way the light travels; unused by a point light
	float  cone_scale;  // the spot cone, as a remap of the cosine off the light's axis (see sample())
	float3 color;       // linear colour times intensity
	float  cone_offset;
	float  falloff;     // 1 / source radius squared
	uint   flags;       // the scene's; nothing here reads them
	uint   shadow;      // its shadow map's record, plus one; zero for none
	uint   _pad0;
};

/// Mirror of render::ShadowData: one shadow map as the lighting feature publishes it.
public struct ShadowData
{
	float4x4 world_to_map; // world to the map's (u, v, depth), depth biased toward the light
	uint     map;          // the depth texture
	float    size;         // texels across
	uint     _pad0;
	uint     _pad1;
};

[[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<LightData>  g_lights[];
[[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] StructuredBuffer<ShadowData> g_shadows[];

/// How much of a light reaches a point past its shadow map: 1 lit, 0 in shadow. One texel, compared
/// once: shading points on the art's texel centres make the edge land on the art's pixels, hard,
/// the way a pixel artist draws a cast shadow.
float shadow_at(ShadowData shadow, float3 position)
{
	const float4 clip = mul(shadow.world_to_map, float4(position, 1.0));
	const float3 map  = clip.xyz / clip.w;

	// Past the map's edge nothing was drawn to cast a shadow.
	if (any(map.xy < 0.0) || any(map.xy >= 1.0))
		return 1.0;

	// The light's index is the same across the wave, so this one is uniform. The map holds the
	// nearest caster's depth, and under reverse Z nearer is greater: a point is lit when it is at
	// least as near as whatever was drawn there.
	const float caster = texture_2d(shadow.map).Load(int3(int2(map.xy * shadow.size), 0)).r;
	return map.z >= caster ? 1.0 : 0.0;
}

/// One light as it arrives at a point.
public struct LightSample
{
	float3 direction;   // unit, from the point toward the light
	float3 color;       // the light's colour times its intensity
	float  attenuation; // distance falloff times the spot cone: 1 at full strength
	float  shadow;      // the fraction the shadow lets through

	/// What reaches the point, before the surface's angle to it.
	float3 radiance() { return color * (attenuation * shadow); }
};

/// The 4x4 ordered-dither threshold of a cell, in (0, 1). Sixteen evenly spaced values arranged so
/// neighbours differ as much as possible, so any mix of two tones reads as an even stipple.
float bayer4(int2 cell)
{
	static const uint MATRIX[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

	// Two's complement keeps negative cells tiling: -1 & 3 is 3.
	const uint2 p = uint2(cell) & 3;
	return (MATRIX[p.y * 4 + p.x] + 0.5) / 16.0;
}

/// The scene's light as one shading point sees it. The shading model owns the loop over the lights,
/// so a model that ignores light costs nothing and one that quantises can do so light by light.
public struct Lighting
{
	uint count; // lights reaching the point: sample() takes 0 to count - 1

	internal float3 sky;
	internal float3 ground;
	internal uint   table;
	internal uint   first;
	internal uint shadow_table;
	internal uint first_shadow;
	internal uint   bands;
	internal float  threshold; // where band() rounds up here: one half, or the dither cell's value

	/// The ambient light arriving along a direction: a hemisphere light, sky from above and ground
	/// from below. Two colours are the cheapest ambient that still tells up from down, which is most
	/// of what makes a form read as lit rather than flat.
	float3 ambient(float3 direction) { return lerp(ground, sky, direction.y * 0.5 + 0.5); }

	/// A light level in [0, 1] stepped to the frame's bands. The threshold is the dither cell's
	/// value, so across a region the steps average back to the smooth level instead of losing it.
	float band(float level) { return bands == 0 ? level : floor(level * bands + threshold) / bands; }

	LightSample sample(uint index, float3 position)
	{
		const LightData light = g_lights[table][first + index];

		LightSample result;
		result.color       = light.color;
		result.attenuation = 1.0;
		result.shadow      = 1.0;

		if (light.shadow != 0)
			result.shadow = shadow_at(g_shadows[shadow_table][first_shadow + light.shadow - 1], position);

		// A directional light is the same everywhere. The branch is uniform per light, so it costs
		// no divergence.
		if (light.range == 0.0)
		{
			result.direction = -light.direction;
			return result;
		}

		const float3 to_light    = light.position - position;
		const float  distance_sq = dot(to_light, to_light);
		result.direction         = to_light * rsqrt(max(distance_sq, 1e-8));

		// Inverse square, windowed to reach exactly zero at the range so a light's influence has a
		// hard edge the clustering can trust (Karis, Real Shading in Unreal Engine 4). Distance is
		// measured in source radii, and the +1 keeps the light finite at its source, where it equals
		// the intensity. All in squared distance: no square root, no pow.
		const float ratio  = distance_sq / (light.range * light.range);
		const float window = saturate(1.0 - ratio * ratio);

		// Squared, the linear remap eases in at the cone's edge. A point light's 0 and 1 make it 1
		// with no branch.
		const float cone = saturate(dot(-result.direction, light.direction) * light.cone_scale + light.cone_offset);

		result.attenuation = window * window / (distance_sq * light.falloff + 1.0) * (cone * cone);
		return result;
	}
};

/// The scene's light as it reaches a point: the one surface() chose to shade at.
public Lighting lighting_at(float3 position)
{
	// Dither cells are squares of the world in the view's plane, not screen pixels, so the stipple
	// holds still as the camera moves, and one texel across it lands on the art's pixels at any
	// screen scale. A w of zero keeps the view's rotation and drops its translation.
	const float2 cell = floor(mul(g_view.view, float4(position, 0.0)).xy / g_frame.lighting.dither_cell);

	Lighting lighting;
	lighting.count     = g_frame.lighting.light_count;
	lighting.sky       = g_frame.lighting.sky;
	lighting.ground    = g_frame.lighting.ground;
	lighting.table     = g_frame.lighting.lights;
	lighting.first     = g_frame.lighting.first_light;
	lighting.shadow_table = g_frame.lighting.shadows;
	lighting.first_shadow = g_frame.lighting.first_shadow;
	lighting.bands     = g_frame.lighting.bands;
	lighting.threshold = lerp(0.5, bayer4(int2(cell)), g_frame.lighting.dither);
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

/// Light in steps, the way a pixel artist shades by hand. Each light's level is stepped before the
/// sum, so light pools and the terminator on a form break into a few flat tones, and dithering
/// between two tones stands in for the ones between. Diffuse only: at pixel-art resolution a
/// highlight is a pixel the artist places. The ambient stays smooth, the base the steps sit on.
public struct Pixel : IShadingModel
{
	static float3 shade(Surface s, SurfaceInput i, Lighting lighting)
	{
		float3 incoming = lighting.ambient(s.normal) * s.occlusion;

		for (uint index = 0; index < lighting.count; ++index)
		{
			const LightSample light = lighting.sample(index, s.shading_position);
			const float       level = saturate(dot(s.normal, light.direction)) * light.attenuation * light.shadow;

			incoming += light.color * lighting.band(level);
		}

		return s.albedo * incoming + s.emissive;
	}
};

/// GGX's distribution, the height-correlated Smith visibility and Schlick's Fresnel: the specular
/// BRDF every current engine shares (Filament's formulation), so art made for them matches here.
/// Returned times pi, the light units' factor, which cancels the distribution's own 1/pi.
float3 specular(float NoL, float NoV, float NoH, float VoH, float a2, float3 f0)
{
	const float  d          = NoH * NoH * (a2 - 1.0) + 1.0;
	const float  D          = a2 / (d * d);
	const float  visibility = 0.5 / (NoL * sqrt(NoV * NoV * (1.0 - a2) + a2) + NoV * sqrt(NoL * NoL * (1.0 - a2) + a2));
	const float3 F          = f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0);

	return D * visibility * F;
}

/// The split sum's scale and bias on f0, Karis's analytic fit to the integrated BRDF (Physically
/// Based Shading on Mobile): within a few percent of the lookup table, with no texture to bind.
float3 environment_brdf(float3 f0, float roughness, float NoV)
{
	const float4 r    = roughness * float4(-1.0, -0.0275, -0.572, 0.022) + float4(1.0, 0.0425, 1.04, -0.04);
	const float  a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	const float2 ab   = float2(-1.04, 1.04) * a004 + r.zw;

	return f0 * ab.x + ab.y;
}

/// Physically based: Lambert diffuse under a GGX specular lobe, metallic workflow. Roughness is
/// perceptual, squared into GGX's alpha, and non-metals reflect 4% head on. The ambient is the
/// hemisphere seen along the normal for diffuse and along the reflection for specular.
public struct Standard : IShadingModel
{
	static float3 shade(Surface s, SurfaceInput i, Lighting lighting)
	{
		const float3 n = s.normal;
		const float3 v = i.view_dir;

		// A normal map can tip n away from the eye, and NoV divides below.
		const float NoV = max(dot(n, v), 1e-4);

		// Alpha zero is a mirror, whose highlight is infinitely bright and one texel wide.
		const float a  = max(s.roughness * s.roughness, 2e-3);
		const float a2 = a * a;

		const float3 diffuse = s.albedo * (1.0 - s.metallic);
		const float3 f0      = lerp(0.04, s.albedo, s.metallic);

		float3 direct = 0.0;

		for (uint index = 0; index < lighting.count; ++index)
		{
			const LightSample light = lighting.sample(index, s.shading_position);

			// The half vector of a light straight behind the surface is zero: max() keeps it finite,
			// and NoL zeroes that light anyway.
			const float3 sum = light.direction + v;
			const float3 h   = sum * rsqrt(max(dot(sum, sum), 1e-8));

			const float NoL = saturate(dot(n, light.direction));
			const float NoH = saturate(dot(n, h));
			const float VoH = saturate(dot(v, h));

			direct += light.radiance() * NoL * (diffuse + specular(NoL, NoV, NoH, VoH, a2, f0));
		}

		const float3 ambient = lighting.ambient(n) * diffuse +
							   lighting.ambient(reflect(-v, n)) * environment_brdf(f0, s.roughness, NoV);

		return direct + ambient * s.occlusion + s.emissive;
	}
};
