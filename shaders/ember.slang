#pragma once

// The shader half of ember's bindless contract. include/ember/gpu/common.h is the C++ half.
// The binding numbers and array shapes here mirror its BINDING_* constants and must match.
//
// Set 0 is the bindless heap: every live texture, sampler, and storage buffer, at the slot
// its pool handle names. The device binds it once per frame; shaders reach any resource from
// a plain uint. Slot 0 of every array holds a device fallback; so a stale or zero index reads
// the fallback (magenta in dev builds, white in ship) instead of faulting.

// Sampled textures, one array per view type. An index is only valid in the array matching its
// texture's TextureType.
[[vk::binding(0, 0)]] Texture2D      ember_textures_2d[];
[[vk::binding(1, 0)]] Texture2DArray ember_textures_2d_array[];
[[vk::binding(2, 0)]] TextureCube    ember_textures_cube[];
[[vk::binding(3, 0)]] Texture3D      ember_textures_3d[];

// Storage arrays split by view dimensionality so every slot, including reset
// ones, holds a descriptor matching the declared type. Cube storage lives in
// the 2D array binding.
[[vk::binding(4, 0)]] RWTexture2D<float4>      ember_storage_2d[];
[[vk::binding(5, 0)]] RWTexture2DArray<float4> ember_storage_2d_array[];
[[vk::binding(6, 0)]] RWTexture3D<float4>      ember_storage_3d[];

[[vk::binding(7, 0)]] SamplerState ember_samplers[];

[[vk::binding(8, 0)]] ByteAddressBuffer ember_buffers[];

#define EMBER_BUFFER_ALIAS(T, name) [[vk::binding(8, 0)]] StructuredBuffer<T> name[];

// Accessors stay macros so NonUniformResourceIndex lands textually on the descriptor
// access; a function parameter can lose the qualifier to inlining.
//
// The short names assume a dynamically uniform index (push constants, per draw
// data). Reach for the _nu forms when the index can vary inside a wave (per pixel
// material ids); a divergent index without the qualifier is undefined and shows up
// as sparkle on AMD.
#define EMBER_TEX2D(i)       ember_textures_2d[i]
#define EMBER_TEX2D_ARRAY(i) ember_textures_2d_array[i]
#define EMBER_TEXCUBE(i)     ember_textures_cube[i]
#define EMBER_TEX3D(i)       ember_textures_3d[i]
#define EMBER_STORAGE_2D(i)  ember_storage_2d[i]
#define EMBER_SAMPLER(i)     ember_samplers[i]
#define EMBER_BUFFER(i)      ember_buffers[i]

#define EMBER_TEX2D_NU(i)       ember_textures_2d[NonUniformResourceIndex(i)]
#define EMBER_TEX2D_ARRAY_NU(i) ember_textures_2d_array[NonUniformResourceIndex(i)]
#define EMBER_TEXCUBE_NU(i)     ember_textures_cube[NonUniformResourceIndex(i)]
#define EMBER_TEX3D_NU(i)       ember_textures_3d[NonUniformResourceIndex(i)]
#define EMBER_STORAGE_2D_NU(i)  ember_storage_2d[NonUniformResourceIndex(i)]
#define EMBER_SAMPLER_NU(i)     ember_samplers[NonUniformResourceIndex(i)]
#define EMBER_BUFFER_NU(i)      ember_buffers[NonUniformResourceIndex(i)]

// Set 1: three constant slots with dynamic offsets, the data path for CPU issued
// draws. Slot use by frequency is the convention: 0 per frame, 1 per pass, 2 per
// draw. Declare a typed block for each slot the shader reads; CommandList
// set_constants fills it from the transient ring.
#define EMBER_CONSTANTS(slot, T, name) [[vk::binding(slot, 1)]] ConstantBuffer<T> name;

// Writable view over the same storage buffer heap, for compute producers
// (draw stream compaction, scatter uploads). Vulkan binds StructuredBuffer and
// RWStructuredBuffer through the same descriptor type, so the slot contract is
// unchanged; only shader-side access widens.
#define EMBER_BUFFER_RW_ALIAS(T, name) [[vk::binding(8, 0)]] RWStructuredBuffer<T> name[];
