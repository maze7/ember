import material;

/// Texture times tint times vertex colour, lit by nothing: terrain tiles, signs, UI in the world,
/// bring-up. Opaque, so art with holes belongs to the sprite type, which cuts them.
[Shading("unlit")]
struct Unlit : IMaterial
{
	[Filter("point")] Texture2DRef albedo;
	[Color] [Default("1 1 1 1")] float4 tint;

	void surface(SurfaceInput s, inout Surface out)
	{
		out.albedo = (albedo.sample(s.uv) * tint * s.color).rgb;
	}
};
