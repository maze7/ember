#language slang 2026

// The shader half of ember's bindless contract. include/ember/gpu/common.h is the C++ half: every
// number here mirrors a constant there, and changing one side means changing both.
//
// Set 0 is the bindless heap. Every live texture, sampler and storage buffer sits at the slot its
// pool handle names, so a shader reaches any resource from a plain uint and nothing is ever bound
// per draw. Slot 0 of every array holds a device fallback (magenta in dev builds, white in ship),
// so a stale or zero index reads something visible instead of faulting.
//
// Set 1 holds three constant blocks with dynamic offsets, one per update frequency. A program
// declares the typed block it reads at the slot matching how often that data changes.
module ember;

public static const uint SET_HEAP      = 0;
public static const uint SET_CONSTANTS = 1;

public static const uint BINDING_SAMPLED_2D       = 0;
public static const uint BINDING_SAMPLED_2D_ARRAY = 1;
public static const uint BINDING_SAMPLED_CUBE     = 2;
public static const uint BINDING_SAMPLED_3D       = 3;
public static const uint BINDING_STORAGE_2D       = 4;
public static const uint BINDING_STORAGE_2D_ARRAY = 5;
public static const uint BINDING_STORAGE_3D       = 6;
public static const uint BINDING_SAMPLERS         = 7;
public static const uint BINDING_STORAGE_BUFFERS  = 8;

public static const uint CONSTANTS_FRAME = 0;
public static const uint CONSTANTS_PASS  = 1;
public static const uint CONSTANTS_DRAW  = 2;

// The heap, declared once so every program that imports ember shares one set of arrays. They stay
// internal: code reaches them through the accessors below, so every call site has to say whether
// its index is uniform, which is the one question here that decides correctness.
[[vk::binding(BINDING_SAMPLED_2D, SET_HEAP)]]       Texture2D      g_textures_2d[];
[[vk::binding(BINDING_SAMPLED_2D_ARRAY, SET_HEAP)]] Texture2DArray g_textures_2d_array[];
[[vk::binding(BINDING_SAMPLED_CUBE, SET_HEAP)]]     TextureCube    g_textures_cube[];
[[vk::binding(BINDING_SAMPLED_3D, SET_HEAP)]]       Texture3D      g_textures_3d[];

// Storage views split by dimensionality so every slot, reset ones included, holds a descriptor of
// the declared type. Cube storage views live in the 2D array binding.
[[vk::binding(BINDING_STORAGE_2D, SET_HEAP)]]       RWTexture2D<float4>      g_storage_2d[];
[[vk::binding(BINDING_STORAGE_2D_ARRAY, SET_HEAP)]] RWTexture2DArray<float4> g_storage_2d_array[];
[[vk::binding(BINDING_STORAGE_3D, SET_HEAP)]]       RWTexture3D<float4>      g_storage_3d[];

[[vk::binding(BINDING_SAMPLERS, SET_HEAP)]] SamplerState g_samplers[];

// The untyped view of the storage buffers. Typed views of the same binding are declared by the
// module that owns the element type (render.slang, the entry modules), since Slang has no generic
// globals. Vulkan binds StructuredBuffer and ByteAddressBuffer through one descriptor type, so
// every view reads the same buffers.
[[vk::binding(BINDING_STORAGE_BUFFERS, SET_HEAP)]] ByteAddressBuffer g_buffers[];

// ---- Uniform access -----------------------------------------------------------------------------
// For an index every lane of the wave agrees on: push constants, constant blocks, anything that is
// the same for the whole draw.

public Texture2D                texture_2d(uint index)       { return g_textures_2d[index]; }
public Texture2DArray           texture_2d_array(uint index) { return g_textures_2d_array[index]; }
public TextureCube              texture_cube(uint index)     { return g_textures_cube[index]; }
public Texture3D                texture_3d(uint index)       { return g_textures_3d[index]; }
public RWTexture2D<float4>      storage_2d(uint index)       { return g_storage_2d[index]; }
public RWTexture2DArray<float4> storage_2d_array(uint index) { return g_storage_2d_array[index]; }
public RWTexture3D<float4>      storage_3d(uint index)       { return g_storage_3d[index]; }
public SamplerState             sampler(uint index)          { return g_samplers[index]; }
public ByteAddressBuffer        byte_buffer(uint index)      { return g_buffers[index]; }

// ---- Divergent access ---------------------------------------------------------------------------
// For an index that can differ between lanes: material records inside a multi-draw (a wave packs
// pixels from several draws), per-pixel ids. A divergent index without the qualifier is undefined
// behaviour and shows up as sparkle on AMD. The qualifier on an index that happens to be uniform
// costs a scalarisation loop that runs once, so when in doubt, use these.

public Texture2D         texture_2d_nu(uint index)       { return g_textures_2d[NonUniformResourceIndex(index)]; }
public Texture2DArray    texture_2d_array_nu(uint index) { return g_textures_2d_array[NonUniformResourceIndex(index)]; }
public TextureCube       texture_cube_nu(uint index)     { return g_textures_cube[NonUniformResourceIndex(index)]; }
public Texture3D         texture_3d_nu(uint index)       { return g_textures_3d[NonUniformResourceIndex(index)]; }
public SamplerState      sampler_nu(uint index)          { return g_samplers[NonUniformResourceIndex(index)]; }
public ByteAddressBuffer byte_buffer_nu(uint index)      { return g_buffers[NonUniformResourceIndex(index)]; }
