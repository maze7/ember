# Ember Render Module

Revision 2, 2026-09-03. As built through ember `86b1374`, plus the agreed forward
plan. Supersedes the 2026-08-31 revision, which was agreed in session but never
committed; its still-valid sections are folded in here. This document is a map:
each load-bearing invariant has exactly one authoritative home in a code comment,
and the map says where. When the tree and this document disagree, the tree wins
and the document owes a revision.

Sections 1 through 11 describe code that exists. Sections 12 through 14 record
agreed design that is not yet built.

## 01 Purpose and consumers

`ember::render` sits on `ember::gpu`. Three consumers, one module:

- **The game.** 3D scenes drawn to read as 2D pixel art (Eastward, Pathway,
  Romestead as visual targets) with real time sun, local lights and shadows.
  A 3D rebuild of game2: top-down networked action, tilemap world with
  elevation, animated characters, particles, floor decals, day/night.
- **Editor and debug tooling.** game2 already had an in-game editor; the
  rebuild keeps that workflow. ImGui and debug views are first-class consumers.
- **A later PBR demo.** Same scene representation, same graph, same geometry,
  material and visibility plumbing, different feature set and shader library.

The split that makes all three work: everything below the feature layer is
style agnostic. Style lives in features and shader families, so a second
renderer configuration plugs into the same core.

Judging rule: achievable solo, impresses a AAA rendering engineer, ships games.

## 02 Targets and constraints

- Desktop Vulkan 1.3 now, DX12 and consoles later. No mobile, no WebGPU.
- Hardware floor: any Vulkan 1.3 driver. Core features only; optional paths
  gate on caps (`indirect_count`, `sampler_minmax`, `timestamps`).
- Mesh shaders and ray tracing stay out of the core. Candidate extensions for
  the PBR era.
- Single queue, single threaded recording in v1.
- The renderer is ECS agnostic. The game speaks EnTT; the renderer speaks
  handles (section 15).
- House rules apply: no exceptions, handles and pools, defs with designated
  initializers, data plus free functions, classes only where the litmus test
  passes.

## 03 The pixel contract

The game's look hangs on one invariant: world rendering happens at a low
internal resolution where one screen pixel equals one art texel, and the result
reaches the display through integer or near-integer upscale. This binds the
game's renderer configuration, never the core.

- Internal resolution is a config knob until an on-screen comparison locks it.
  Working default 640x360 (x3 to 1080p, x4 to 1440p, x6 to 4K). `FixedScale`
  default policy; `FixedAspect` per competitive mode.
- Camera snapping: the view origin snaps to the texel grid, the sub-texel
  remainder rides into the upscale pass as a UV offset, one pixel gutter per
  edge. Ortho main camera; the perspective debug fly camera ships beside it
  with snapping off.
- Forbidden: MSAA, TAA, DLSS/FSR, filtered albedo. Point sampling always;
  mips exist for minification stability and stay point sampled per level.
- Stylization toolkit, all knobs: per-texel shading, quantized light ramps,
  chunky shadows, outline pass, posterize plus dither.
- Non-integer leftovers go through sharp-bilinear upscale; integer scales
  reduce to pure nearest.

These land in the pixel presentation and stylized forward features. The core
sections below never reference them.

## 04 Architecture

Dependencies point down only:

    game / editor
        RenderScene            CPU proxies, dirty tracking
        GpuScene               persistent GPU tables, dirty sync
        GeometryPool           shared vertex/index streams + geometry table
        MaterialPool(s)        per shader family material tables
        View + Visibility      frustum values, cull compute, draw streams
        [features]             agreed, section 12; the only style-aware layer
        RenderGraph            passes, transients, derived barriers
        ember::gpu             RHI

Mechanisms versus features is a hard distinction. RenderScene, GpuScene,
GeometryPool, MaterialPool, views, visibility and the graph are always present
and never optional. Features (shadows, clustered lighting, forward shading,
sprites, bloom, presentation) are composed per game. There is no
`GpuSceneFeature`; mechanisms do not wear the feature interface.

Scale note, stated honestly: at cozy-game scale the whole frame is cheap on any
desktop GPU. The architecture buys the portfolio story and the ceiling: the
same core must hold when the PBR demo raises resolution to native and the
instance count by orders of magnitude. The 100k-cube runner exists to prove
that ceiling early.

## 05 RenderScene

`render/scene.h`. Renderer-owned proxy storage: the game mirrors whatever it
considers renderable into objects here, and the renderer never sees game
entities. 500 identical crates are 1 geometry, 1 material, 500 objects.

- One generational pool (`Pool`, u32 component). Hot values are the exact GPU
  `ObjectData` records; cold values carry the packed transform and local
  sphere. GPU sync is therefore a straight copy of pool storage.
- A handle's index is the object's slot in every GPU table. This extends the
  engine's standing invariant (pool index == bindless slot) up a layer, and it
  is why capacity is fixed at init.
- One dirty stream (bitset plus dense list) covers both tables. Transform
  changes rewrite the world-space cull sphere in the object record anyway, so
  split streams would only save the rare material-only edit.
- `destroy_object` scrubs the record to zeros (layer mask zero is the GPU-side
  dead marker) while the slot is live, marks it dirty, then erases. Slot
  storage outlives the handle, so sync uploads the scrub from the dead slot.
- `slot_count()` is a monotonic high-water mark. GPU tables and cull dispatch
  bounds must keep covering scrubbed slots, so the bound never shrinks.
- Setters assert on stale handles in debug and no-op in release; a set on a
  destroyed proxy is a game lifetime bug.

Bounds are proxy data: the def carries a local sphere, the scene keeps world
spheres current on transform writes (conservative under non-uniform scale).
RenderScene never touches the device or the geometry pool.

## 06 GpuScene and synchronization

`render/gpu_scene.h`. Persistent `ObjectData` and `TransformData` tables, one
slot per scene slot, written only by `sync()`.

`sync()` drains the scene's dirty list, sorts it, and stages one copy per
consecutive slot run per table. Object records upload straight out of pool hot
storage (a slot run is contiguous bytes there); transforms gather once into
frame-arena scratch because they sit inside the cold stride. `SyncStats`
reports dirty slots, runs, copy commands and bytes.

The ordering invariant, whose authoritative home is the `GpuScene` class
comment together with the staging batch barrier comment in the backend: uploads
ride the staging ring inside the frame's command stream; the batch entry
barrier (`ALL_COMMANDS -> COPY`) orders every prior GPU read before the copies,
and the exit barrier publishes the bytes to this frame's work. Scrubs, creates
and same-frame slot reuse land exactly once, in submission order, so the mirror
needs no slot quarantine and no per-frame versioning anywhere.

Consequences worth naming:

- The tables are not graph resources. Culling reads them bindlessly; there is
  nothing for the graph to order until an in-frame GPU pass writes them. The
  day that pass exists (scatter uploads, GPU-driven LOD writeback), that writer
  imports the buffers and pays the barrier cost explicitly.
- The designed upgrade path if coalesced staging ever shows up in a profile is
  a transient-ring scatter compute driven by the same dirty list. The dirty
  list survives; only the writer changes.

## 07 Geometry

`render/geometry.h`. Persistent mesh storage: positions (float4), attributes
(16 bytes: octahedral normal, rgba8 color, uv), u32 indices, plus a
`GeometryData` table, all in four shared device buffers. Draws differ only by
offsets, so one index buffer bind serves every mesh draw and GPU culling writes
indirect arguments from the table alone.

- Indices are rebased to pool-global vertex ids at upload. `base_vertex` is
  always zero and `SV_VertexID` addresses the shared streams directly, which
  retires the entire Slang base-vertex semantics hazard from the GPU-driven
  path. The cost is u32 indices only; revisit only if index memory ever shows
  in a capture.
- Geometries are immutable: create uploads everything including the table
  record, destroy frees the ranges and scrubs the record to `index_count`
  zero so a stale object reference degenerates to an empty draw. Tilemap
  chunks rebuild as new geometries and destroy the old handle.
- Range bookkeeping is `containers/range_allocator.h`: address-sorted,
  fully coalescing first-fit free list, reusable for any element space, unit
  tested. Callers keep their own offset and count, so the allocator carries no
  per-allocation metadata.
- Freed ranges are reallocated and rewritten immediately, with no
  frames-in-flight quarantine. Safe for the same reason table sync is safe:
  the staging entry barrier orders prior frames' reads before this frame's
  uploads. Same invariant, same home.

Attribute packing helpers live in `math/packing.h` (octahedral encode and
decode, round-trip tested); the shader-side decode lives with the vertex
pulling code.

## 08 Materials

`render/material.h`. One `MaterialPool` per shader family: a bindless table of
fixed-stride records addressed by `MaterialHandle` index, which is what
`ObjectData.material` carries. The renderer core never reads material bytes;
only the family's own shaders cast them. This is how the core avoids hardcoding
any material model: the stylized family and a future PBR family are two pools
with two record layouts and zero shared field assumptions.

- The def carries an error record of stride bytes that permanently owns slot 0
  and reads as an obvious mistake on screen (fallback texture under a white
  tint). A null handle renders the error material, and destroy writes it over
  freed slots. All-zero is deliberately not used; a zero tint would multiply
  the fallback to black and hide.
- Mutations write CPU shadow records and mark the slot dirty; `sync()` uploads
  each dirty slot once per frame. The single upload point makes edits
  last-write-wins and same-frame destroy-plus-reuse safe, since overlapping
  staged copies within one frame are unordered by the device contract.
- Destroy is diagnostic, never lifetime: a material must outlive every object
  referencing it; after slot reuse a stale reference silently reads the new
  record.
- A "material template" today is a game-owned pairing of one pool, one
  pipeline set and one GPU struct. A template type earns existence when
  families must mix inside one visibility stream; at that point the object's
  material field becomes a packed {family, slot} pair and streams bucket on
  the family bits. Nothing in `MaterialPool` changes for that.

Shader rule with its home in `ember.slang`: material-derived texture and
sampler indices vary per draw inside an indirect batch and waves can pack
across draws, so consuming shaders use the `_NU` accessor forms.

## 09 Views

`render/view.h`. A view is a value; main camera, shadow cascade, reflection,
editor picking and minimap are all `View`, and the machinery never asks which.
`make_view(view, projection, extent, layers, name)` is the one builder;
features derive secondary views from the main one.

- Frustum: six world-space inward planes via Gribb-Hartmann in Vulkan clip
  conventions. Planes with a normal are normalized so tests work in world
  units; the infinite-far reverse-Z projection yields a far plane with zero
  normal and positive w, which every sphere passes. That is the meaning of far
  at infinity, and it keeps finite-far views (shadow orthos) on the identical
  code path.
- `render::intersects` is the CPU reference implementation of the cull test;
  the compute kernel mirrors it and the runner counts both for A/B.
- `perspective_reverse_z`: infinite far, depth clears to zero, GreaterEqual.
- Constants convention: slot 0 carries the frame block with the main view,
  secondary views ride slot 1 per-pass blocks, cull dispatches receive their
  frustum through per-dispatch slot 1 constants.
- Layer masks select: views carry a `LayerMask`, objects belong to layers, a
  view draws an object when the masks intersect. Layer zero on an object means
  invisible everywhere, which is how dead slots read.

## 10 Visibility and draw streams

`render/visibility.h`. The contract: visibility produces `DrawStream`s, raster
passes consume them, and a consumer never knows whether a stream came from
frustum culling, occlusion, LOD selection or meshlets. Today `Visibility::cull`
is frustum-only; HZB, LOD and meshlet stages change its internals and nothing
downstream.

- `DrawStream` = compacted `DrawIndexedIndirectArgs` graph buffer + count graph
  buffer + `max_draw_count` (the tightest bound the producer can emit) +
  `use_count`. Consumers declare reads via `render::read(pass, stream)` and
  issue via `draw_indexed_stream(cmd, ctx, stream)`.
- The cull kernel (assets cook, `cull.slang`): one thread per slot, layer gate
  (retires dead slots and applies view selection in one test), sphere versus
  six planes mirroring the CPU reference, geometry table fetch, empty-geometry
  gate, `InterlockedAdd` compaction. Compaction order is GPU-scheduling
  dependent; fine for depth-tested opaque, known before it surprises anyone.
- Draw identity: the object slot rides `first_instance`; the vertex stage
  recovers it as `SV_InstanceID + SV_StartInstanceLocation`. Slang lowers
  system values with D3D semantics (base subtracted) on every target, proven
  from cooked SPIR-V during the ImGui bring-up, so implicit identity through
  `first_instance` alone never reaches the shader. `shaderDrawParameters` is
  pinned at boot; verified cooking and running. DX12 before SM6.8 needs the
  ExecuteIndirect root-constant scheme; port-day answer.
- Without `caps.indirect_count`, the clear pass zero-fills the args buffer and
  the consumer draws `max_draw_count`; the zeroed tail draws empty. Consumers
  never branch; `use_count` is baked into the stream.
- `VisibilityReadback` is separate instrumentation: per frame-slot,
  per-query-slot count copies (`VISIBILITY_QUERY_SLOTS` = 8, shaped for
  cascades), read `frames_in_flight` later. A statistic, never a rendering
  input.

Streams are graph transients, so clear, cull write, indirect consumption and
readback copies all get derived barriers, and the buffers recycle through the
graph pool across frames.

## 11 Render graph

`render/graph.h`. Declared fresh each frame; passes name reads and writes with
explicit states, the graph pools transient textures and buffers, derives
barriers from state pairs against the steady-layout resting contract, wraps
passes in GPU zones, executes in declaration order into one command list.

v1 scope caps, on purpose: no aliasing, no reordering, no async, no retained
caching. Contract limits (64 passes, 64 textures, 64 buffers, 15 uses per
pass) are raised when a consumer outgrows them. Record captures live in the
frame arena and must be trivially destructible.

## 12 Feature composition (agreed, not yet built)

Settled 2026-09-03 after debating a declare-style alternative (see section 19).
To land as the extraction commit: the runner's inline frame moves into the
first feature module and the composition types below appear.

- `RenderFeature`: virtual interface, `build_views(RenderFrame&)` and
  `add_passes(RenderFrame&)`, plus `shutdown(Device&)`. Frame-graph
  granularity, cold by construction; the legitimate home for dynamic dispatch.
- `Renderer`: owns GpuScene, Visibility, the graph and the feature list.
  `add_feature<F>(F::Def)` constructs with the device and returns typed `F&`
  so games keep handles for runtime knobs. `render(scene, main_view, output)`
  runs the fixed phase contract: sync, build views (main view is id 0), cull
  every view, `add_passes` in registration order, execute. RenderScene and the
  geometry/material pools stay outside, referenced by `RendererDef`; they
  outlive renderer policy and serve the editor without one.
- `RenderFrame`: device, scene, gpu scene, graph, the view list with parallel
  per-view visibility, and `FrameResources`.
- `FrameResources`: a plain struct with typed members for Ember-standard
  semantic resources (scene color, scene depth, imported output, sun shadow
  data, lighting data, later HiZ). Producers assign, consumers read; a null
  handle means no registered feature produces it, and optional consumers fall
  back while required consumers assert at the use site.
- Explicitly absent: string resource ids, service locators, ViewBuilder,
  arbitrary hook points. Game-custom features that share data are both
  constructed by the game, so they share via constructor injection; the
  engine blackboard carries only Ember-standard semantics.
- Registration order is pass order. That is a documented contract, not an
  accident, and the standard order lives in one snippet games copy.

Red team, on record: wiring errors surface at runtime (mitigated by null
asserts and a debug post-pass check), registration order carries meaning and
will eventually bite a mid-list insertion, and `FrameResources` will feel
growth pressure. The membership tripwire: would a second game ever publish
this member. If features ever need to negotiate ordering among themselves,
that is the evidence threshold for real dependency declarations.

## 13 Lighting and shadows (plan)

Carried from revision 1; numbers stand until a stage proves otherwise.

- Clustered forward. One lit-opaque shader family per style; a compute pass
  culls lights into a view-space froxel grid (16x9 tiles x 24 slices, linear
  mapping under the ortho main camera, slice function behind one helper for
  the perspective era); budgets 256 visible lights, 32 per cluster. Both the
  stylized and PBR families consume the same light lists.
- Sun: one stable orthographic shadow map, 2048 D32, Valient snap recipe,
  3x3 PCF through a compare sampler, GreaterEqual under reverse-Z,
  receiver-side normal-offset bias plus pipeline depth bias.
- RHI prerequisite before the shadow feature: depth bias fields on
  `GraphicsPipelineDef` (recorded debt).
- Lights carry HDR linear color with intensity premultiplied; HDR end to end,
  tonemap once, sRGB at the swapchain.
- Shadow views are ordinary `View`s culled by the ordinary path into ordinary
  streams; `VisibilityReadback` query slots already cover cascades.

## 14 2D, Canvas and ImGui (plan and landed)

- World-space flats are 3D citizens: quad geometries with cutout materials,
  the `Billboard` object flag when they face the camera, lit and shadowed like
  everything else. Floor decals are biased ground quads. game2's blob shadows
  and painter's y-sort retire.
- Canvas: the game2 Batcher API ports nearly verbatim on rebuilt internals
  (bindless kills texture batch-breaks, transient ring kills owned meshes,
  the graph kills render-target plumbing, freeform ~80 byte quad records with
  vertex-pulled corners). Two mounts: `canvas_world` at internal resolution
  before upscale, `canvas_screen` at native after it. Text v1 is a baked
  bitmap font.
- ImGui: landed as `Ember::ImGui` (custom input bridge, custom RHI backend,
  `ImTextureID` is a `TextureHandle`, dynamic-texture protocol). Out of ship
  builds by linkage.

## 15 The ECS boundary

The game speaks EnTT; the renderer speaks handles; one game-side sync layer
connects them.

    struct RenderProxy { render::RenderObjectHandle object; };
    // on_construct/on_destroy hooks manage proxies; per frame, interpolation
    // writes RenderTransform and the sync loop calls scene.set_transform.

Why the indirection: gameplay wants components while the GPU wants densely
packed records at stable indices (EnTT entity ids recycle and its storage
reorders); the module boundary keeps the editor, the PBR demo and any future
project driving the renderer without EnTT; and the proxy layer is the extract
boundary that later lets simulation tick N+1 overlap rendering frame N.
Cost: one copy per changed proxy per frame; noise at this scale.

## 16 GPU data reference

All GPU-shared structs keep sizes at multiples of 16 bytes (std430 stride
rule, learned from the ImGui 20-byte vertex); `DrawArgs` at 20 bytes is the
deliberate exception because it must be bit-identical to the API struct.
C++ and Slang mirrors are hand-maintained with static_asserts on size.

| Struct | Size | Home | Notes |
|---|---|---|---|
| ObjectData | 32 | scene.h | world sphere, geometry, material, flags, layers (0 = dead) |
| TransformData | 48 | scene.h | rows of the world 3x4, row major |
| GeometryData | 32 | geometry.h | first_index, index_count (0 = dead), first_vertex, vertex_count, local sphere |
| AttributeData | 16 | geometry.h | oct normal u32, rgba8 u32, uv float2 |
| DrawIndexedIndirectArgs | 20 | gpu | bit-identical to VK and D3D12 |
| material records | n x 16 | per family | opaque to the core |

Conventions: push constants carry bindless indices and small per-dispatch
data within the 32-byte budget; constants slot 0 per frame, slot 1 per pass,
slot 2 free for game code; `EMBER_BUFFER_ALIAS` for typed reads,
`EMBER_BUFFER_RW_ALIAS` for compute writers, `_NU` accessors wherever an
index can diverge inside a wave.

## 17 Debug tooling

The runner doubles as the renderer's harness: F toggles the fly camera
(seeded from the orbit view), freeze-cull-view holds the culling frustum
while the camera moves, the CPU reference counter A/Bs the GPU readback
number, churn recreates one object per frame to soak slot reuse, scatter
stress loads the sync path, P prints GPU zones, T destroys a live texture to
prove the fallback chain, and material edits ride `MaterialPool::update` to
prove the indirection. These stay cheap and stay in.

## 18 Roadmap

Built (ember commits, 2026-09-01..03): pool handle widening (`fad428c`,
`6314948`), RenderScene (`11db17c`), GeometryPool + packing (`1093362`),
GpuScene (`8a740e9`), views (`931dfc7`), visibility (`56c00ce`), materials
plus the ALL_TRANSFER barrier fix (`86b1374`). The 100k-cube vertical slice
runs culled, instrumented and validation-clean.

Next, in order:

1. Feature extraction: `Renderer`, `RenderFeature`, `RenderFrame`,
   `FrameResources`; the opaque path becomes the first feature (section 12).
2. Sprite feature: quads as scene objects, alpha-test stream bucket,
   `Billboard` flag, frame tables later.
3. Depth-bias fields on `GraphicsPipelineDef`, then the directional shadow
   feature: first secondary views, validates `build_views` and query slots.
4. Clustered lighting (section 13), shading-path independent.
5. Stylized features: stylized forward, bloom, pixel presentation (absorbs
   the internal HDR target, upscale, snapped ortho camera and resolution
   knob), then water, fog, particles, HZB.
6. PBR smoke configuration: a second renderer assembly over the same core to
   expose stylized assumptions. Not a second game.

## 19 Rejected and superseded, with reasons

From this revision:

- Declare-style composition (feature modules wired by a hand-written frame
  function) as the primary API: retracted on developer experience grounds;
  every game re-implements and re-breaks the phase contract, and assembly
  requires dependency knowledge the registry encapsulates. Its two good ideas
  survive inside the registry model: typed resource members over string keys,
  and constructor injection for game-custom sharing.
- Transient full-republish v1 (revision 1's plan): dropped as throwaway work
  once the RHI landed compute, indirect count and graph buffers; persistent
  tables with dirty sync went in from day one.
- `GpuTransform.previous_world`: motion vectors serve features the pixel
  contract bans; a future feature adds a separate parallel buffer.
- `transform_index` indirection on objects: no consumer; transforms live at
  the object's own index.
- `ViewBuilder` / `ViewFamily` / `ViewPurpose`: machinery for constructing
  values; a view is a value.
- String-keyed semantic resources and service objects: replaced by typed
  `FrameResources` members and constructor injection.
- CPU visibility fallback: would never ship; the only gated feature is
  `indirect_count`, whose fallback is the zero-filled args path.
- u16 mesh indices: given up for pool-global rebasing, which retired the
  base-vertex semantics hazard entirely.
- Per-table dirty streams in RenderScene: movement dirties both tables
  anyway; one stream, and split later only if animation-heavy scenes prove
  transform-only traffic dominates.

Standing rejections carried from revision 1: deferred shading now (revisit
for PBR), rendering from the EnTT registry, draw stream between world and
RHI (the RHI already decided direct recording), MSAA/TAA/temporal anything,
mesh shaders, graph aliasing/reordering/async before a stage needs them,
runtime Slang, retained UI graph, stock ImGui backends, porting Batcher
internals as-is.

## 20 References

- Aaltonen, HypeHype mobile rendering architecture, REAC 2023.
- O'Donnell, FrameGraph: extensible rendering architecture in Frostbite, GDC 2017.
- Haar & Aaltonen, GPU-driven rendering pipelines, SIGGRAPH 2015 Advances.
- Olsson, Billeter & Assarsson, Clustered deferred and forward shading, HPG 2012.
- Persson, Practical clustered shading, Avalanche.
- Sousa & Geffroy, Doom 2016: the devil is in the details, SIGGRAPH 2016 Advances.
- Valient, Stable rendering of cascaded shadow maps, ShaderX6.
- Jimenez, Next generation post processing in Call of Duty: Advanced Warfare, SIGGRAPH 2014.
- Tatarchuk, Destiny's multithreaded rendering architecture, GDC 2015.
- Cigolle et al., Survey of efficient representations for independent unit vectors, JCGT 2014.
- t3ssel8r, 3D pixel art devlogs (camera snapping, quantized lighting).
- RetroArch sharp-bilinear shader family.
