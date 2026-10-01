import material;

/// Pixel art on an upright card, or any alpha-tested art. Discard shapes the silhouette, so sprites
/// write depth and draw in any order: standees sort against the world and each other by depth,
/// with no sorting pass. The frame shown is per object, so animation writes Instance data and never
/// touches a material.
[Queue("cutout")] [Cull("back")] [Shading("pixel")]
struct Sprite : IMaterial
{
	[Filter("point")] Texture2DRef albedo;
	[Color] [Default("1 1 1 1")] float4 tint;
	[Range(0, 1)] [Default("0.5")] float cutoff;

	struct Instance
	{
		/// The rect of the atlas this object shows: min uv, then max uv. The default is the whole
		/// texture, which is what a baked standee wants, since its vertices carry atlas uvs.
		[Default("0 0 1 1")] float4 frame;

		/// Hit flash: 0 draws the art, 1 draws it white.
		float flash;
	};

	void surface(SurfaceInput s, inout Surface out)
	{
		let data  = instance<Instance>(s);
		let uv    = lerp(data.frame.xy, data.frame.zw, s.uv);
		let texel = albedo.sample(uv) * tint;

		if (texel.a < cutoff)
			discard;

		out.albedo = lerp(texel.rgb, 1.0, data.flash);
		// out.albedo = lerp(float3(luminance(texel.rgb)), 1.0, data.flash);

		// Light and shadow edges land on the art's texels instead of cutting through them.
		snap_to_texels(s, albedo, uv, out);
	}
};
