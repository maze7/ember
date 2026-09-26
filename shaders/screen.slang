#pragma once
#include "material.slang" // attributes, texture refs, frame and view constants

/// The screen domain: a material that colours every pixel of a target. Post effects, blits, overlays.
struct ScreenInput
{
	float2 uv;
	float2 pixel;
};

interface IScreenMaterial
{
	float4 pixel(ScreenInput s);
};

/// Reads the pass inputs the feature bound for this material.
float4 scene_color(float2 uv) { return EMBER_TEX2D(g_view.scene_color).SampleLevel(EMBER_SAMPLER(1), uv, 0.0); }
float  scene_depth(float2 uv) { return EMBER_TEX2D(g_view.scene_depth).SampleLevel(EMBER_SAMPLER(0), uv, 0.0).r; }
