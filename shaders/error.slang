#include "material.slang"

// The engine's error material: what a null or stale MaterialId draws, and what a type whose
// shader failed its first compile shows until the file is fixed. Unlit magenta reads as a
// mistake on any screen; nothing else may look like it.
[Queue("opaque")] [Cull("none")] [Capacity(2)]
struct Material : IMaterial
{
	[Display("Color")] [Color(false)] [Default("1 0 1 1")] float4 color;

	void surface(SurfaceInput s, inout Surface out)
	{
		out.emissive = color.rgb;
		out.shading  = SHADING_UNLIT;
	}
};
