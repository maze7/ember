import material;

/// What a null or stale material handle draws, and what a type shows until its first compile
/// succeeds. A screen-space checker in magenta reads as a mistake on any art, and it does not
/// depend on the mesh's uvs, which may be what is broken.
[Queue("opaque")] [Cull("none")] [Shading("unlit")]
struct Error : IMaterial
{
	[Color] [Default("1 0 1 1")] float4 color;

	void surface(SurfaceInput s, inout Surface out)
	{
		const uint2 cell = uint2(s.pixel) / 8;
		out.albedo = color.rgb * ((cell.x + cell.y) % 2 == 0 ? 1.0 : 0.2);
	}
};
