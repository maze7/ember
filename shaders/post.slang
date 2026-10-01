import ember;

// The screen feature's own passes: bloom's chain down and back up, and the grade, which folds the
// bloom, exposure, a highlight shoulder and two colour lookup tables into the scene in one pass.
// Every pass draws one triangle over its target and reads its sources through the heap.

/// What differs between passes, mirrored in render/features/screen.cpp. One block for every entry
/// point: the textures are the graph's, which only exist by the time a pass records.
struct PostPush
{
	uint   source;    // what the pass filters: the scene, or a bloom level
	uint   bloom;     // the grade's: the bloom chain's top level, zero with bloom off
	uint   sampler;   // linear, clamped to the edge
	float  threshold; // the first step down's: light below it does not bloom
	float2 texel;     // one texel of what the pass filters, in uv
};

/// The grade's knobs, mirrored in render/features/screen.cpp.
struct GradeConstants
{
	float bloom;    // the chain sums its levels, so this is the intensity over their count
	float exposure;
	float knee;     // where the shoulder starts; 1 is a plain clip
	float blend;    // from the first table toward the second

	uint2  luts;      // bindless strips, zero for none
	float2 lut_sizes; // cells along each of their axes
};

[[vk::push_constant]] ConstantBuffer<PostPush> g_push;
[[vk::binding(CONSTANTS_PASS, SET_CONSTANTS)]] ConstantBuffer<GradeConstants> g_grade;

struct Interpolants
{
	float4 position : SV_Position;
	float2 uv       : TEXCOORD0;
};

[shader("vertex")]
Interpolants vs_main(uint vertex_id : SV_VertexID)
{
	// One triangle over the target, as screen.slang draws it: the viewport is flipped, so y flips on
	// the way out, and v runs down the texture as its rows do.
	const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);

	Interpolants output;
	output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	output.uv       = uv;
	return output;
}

float3 fetch(uint texture, float2 uv)
{
	return texture_2d(texture).SampleLevel(sampler(g_push.sampler), uv, 0.0).rgb;
}

/// The light over the threshold, taken evenly from all three channels by the brightest, so the glow
/// keeps the hue of what casts it: a warm pool blooms warm rather than red. Zero passes it all.
float3 over_threshold(float3 color)
{
	if (g_push.threshold <= 0.0)
		return color;

	const float peak = max(color.r, max(color.g, color.b));
	return color * (max(peak - g_push.threshold, 0.0) / max(peak, 1e-4));
}

/// The source at uv, keeping only its light over the threshold.
float3 bright(float2 uv) { return over_threshold(fetch(g_push.source, uv)); }

/// Half the resolution, by Jimenez's thirteen taps (Next Generation Post Processing in Call of Duty:
/// Advanced Warfare, 2014). Each tap sits on a texel corner, so the filter bilinearly averages four
/// texels in one fetch, and the taps form five overlapping boxes: the centre box weighs half and the
/// four around it an eighth each. A lone 2x2 box would alias, and bloom would pulse as a bright
/// texel crossed from one box into the next. The threshold goes on every tap, before the average,
/// so only light over it spreads, never the dark around it.
[shader("fragment")]
float4 fs_down(Interpolants input) : SV_Target
{
	const float2 uv = input.uv;
	const float2 t  = g_push.texel;

	const float3 inner = bright(uv + t * float2(-1.0, -1.0)) + bright(uv + t * float2(1.0, -1.0)) +
						 bright(uv + t * float2(-1.0, 1.0)) + bright(uv + t * float2(1.0, 1.0));

	const float3 corners = bright(uv + t * float2(-2.0, -2.0)) + bright(uv + t * float2(2.0, -2.0)) +
						   bright(uv + t * float2(-2.0, 2.0)) + bright(uv + t * float2(2.0, 2.0));

	const float3 edges = bright(uv + t * float2(0.0, -2.0)) + bright(uv + t * float2(-2.0, 0.0)) +
						 bright(uv + t * float2(2.0, 0.0)) + bright(uv + t * float2(0.0, 2.0));

	// An edge tap belongs to two of the outer boxes and the centre tap to all four.
	const float3 sum = inner * 0.125 + corners * 0.03125 + edges * 0.0625 + bright(uv) * 0.125;
	return float4(sum, 1.0);
}

/// A 3x3 tent over one texel of a smaller level, read at a larger one's pixel: the upsampling filter,
/// smooth enough that the smaller level's texels never show as blocks.
float3 tent(uint texture, float2 uv, float2 t)
{
	const float3 corners = fetch(texture, uv + t * float2(-1.0, -1.0)) + fetch(texture, uv + t * float2(1.0, -1.0)) +
						   fetch(texture, uv + t * float2(-1.0, 1.0)) + fetch(texture, uv + t * float2(1.0, 1.0));

	const float3 edges = fetch(texture, uv + t * float2(0.0, -1.0)) + fetch(texture, uv + t * float2(-1.0, 0.0)) +
						 fetch(texture, uv + t * float2(1.0, 0.0)) + fetch(texture, uv + t * float2(0.0, 1.0));

	return (corners + edges * 2.0 + fetch(texture, uv) * 4.0) * (1.0 / 16.0);
}

/// Back up the chain, blended additively onto the level above, which keeps its own filtered light and
/// gains everything wider: the top ends up holding the sum of every level's blur, a narrow core with a
/// long soft tail, the shape light scattering in a lens has.
[shader("fragment")]
float4 fs_up(Interpolants input) : SV_Target
{
	return float4(tent(g_push.source, input.uv, g_push.texel), 1.0);
}

/// Values up to the knee pass untouched. Past it they roll off toward 1 along an exponential that
/// leaves the line at the same slope, so a highlight keeps its gradation instead of clipping flat. The
/// colours an artist painted sit below the knee, and nothing a pixel-art palette depends on moves.
float3 shoulder(float3 color, float knee)
{
	const float room = 1.0 - knee;

	if (room <= 0.0)
		return min(color, 1.0);

	return min(color, knee) + room * (1.0 - exp(-max(color - knee, 0.0) / room));
}

float3 srgb_from_linear(float3 color)
{
	return select(color <= 0.0031308, color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055);
}

/// A display colour through a lookup strip: `size` slices of size x size side by side, blue choosing
/// the slice, red across it and green down it. Cells are addressed at their centres, so 0 and 1 land
/// on the first and last cells and bilinear filtering never reaches the next slice. Blue falls between
/// two slices and blends them: a 3D texture's trilinear filter, in two fetches of a 2D one. The strip
/// is sRGB, so the hardware decodes each cell before it filters and the result is linear.
float3 lut(uint strip, float size, float3 display)
{
	const float3 cell  = display * (size - 1.0);
	const float  slice = min(floor(cell.b), size - 2.0);

	const float2 texel = float2(slice * size + cell.r + 0.5, cell.g + 0.5);
	const float2 scale = float2(1.0 / (size * size), 1.0 / size);

	const float3 low  = fetch(strip, texel * scale);
	const float3 high = fetch(strip, (texel + float2(size, 0.0)) * scale);
	return lerp(low, high, cell.b - slice);
}

/// The scene as it is shown: its bloom added, exposed, its highlights rolled off, then graded. The
/// tables are indexed by display colour, the space grading tools and artists work in, where 32 cells
/// fall evenly across what the eye tells apart; that needs values in 0..1, which is why grading comes
/// after the shoulder. A table that is missing is the identity, so one table blends in from nothing
/// and no table at all leaves the colour exactly as it was.
[shader("fragment")]
float4 fs_grade(Interpolants input) : SV_Target
{
	const float4 scene = texture_2d(g_push.source).Load(int3(int2(input.position.xy), 0));
	float3 color       = scene.rgb;

	if (g_push.bloom != 0)
		color += tent(g_push.bloom, input.uv, g_push.texel) * g_grade.bloom;

	color = shoulder(max(color * g_grade.exposure, 0.0), g_grade.knee);

	if (any(g_grade.luts != 0))
	{
		const float3 display = saturate(srgb_from_linear(color));
		const float3 from    = g_grade.luts.x != 0 ? lut(g_grade.luts.x, g_grade.lut_sizes.x, display) : color;
		const float3 to      = g_grade.luts.y != 0 ? lut(g_grade.luts.y, g_grade.lut_sizes.y, display) : color;

		color = lerp(from, to, g_grade.blend);
	}

	return float4(color, scene.a);
}
