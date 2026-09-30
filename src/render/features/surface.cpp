#include <ember/gpu/device.h>
#include <ember/memory/pmr/arena.h>
#include <ember/render/features/surface.h>

#include <algorithm>
#include <tuple>
#include <utility>

namespace ember::render
{
	namespace
	{
		/// The only per-draw value: the drawn type's record table. Mirror of surface.slang's
		/// DrawConstants; everything else is per frame or per view.
		struct DrawConstants
		{
			u32 materials;
		};

		/// One bucket as a pass records it.
		struct BucketDraw
		{
			GraphicsPipelineHandle pipeline = {};
			u32 materials					= 0; // the type's record table
			u32 bucket						= 0;
			BucketRange range				= {};
		};

		/// A bucket with objects this frame, and what orders it among the others.
		struct QueuedDraw
		{
			BucketDraw draw		  = {};
			material::Queue queue = material::Queue::Opaque;
			i32 priority		  = 0;
		};
	}

	SurfaceFeature::SurfaceFeature(Renderer& renderer, const Def& def) noexcept
		: m_pipelines(renderer.materials().bucket_count(), BucketPipeline{}, &memory::heap(MemoryTag::Graphics)),
		  m_color_format(def.color_format), m_depth_format(def.depth_format), m_clear(def.clear),
		  m_shadow_bias(def.shadow_bias)
	{
	}

	void SurfaceFeature::shutdown(gpu::Device& device) noexcept
	{
		for (BucketPipeline& entry : m_pipelines)
			destroy(device, entry);
	}

	void SurfaceFeature::prepare(RenderFrame& frame) noexcept
	{
		// A pipeline outlives its type by at most a frame: gone, or no longer a surface type, it goes.
		for (BucketPipeline& entry : m_pipelines)
		{
			if (entry.type.is_null())
				continue;

			const material::Type* type = frame.materials.type(entry.type);

			if (type == nullptr || type->domain != material::Domain::Surface)
				destroy(frame.device, entry);
		}

		// New types and new builds get pipelines; a build that has not moved keeps its own. A build
		// whose pipeline fails keeps its null one until the type moves again, rather than retrying
		// every frame; the device logged why.
		frame.materials.for_each_type(
			[&](MaterialTypeHandle handle, const material::Type& type, u32 generation) noexcept
			{
				if (type.domain != material::Domain::Surface)
					return;

				BucketPipeline& entry = m_pipelines[handle.index];

				if (entry.type == handle && entry.generation == generation)
					return;

				destroy(frame.device, entry);
				entry = {handle, generation, build(frame.device, type), build_shadow(frame.device, type)};
			});
	}

	void SurfaceFeature::add_passes(RenderFrame& frame) noexcept
	{
		// The buckets with objects this frame, in draw order: by queue, which puts opaque before
		// cutout before transparent, then by [Priority], then by bucket, so the order is stable. The
		// casters among them, for the shadow maps, in any order: depth needs none.
		auto* queued = static_cast<QueuedDraw*>(
			frame.scratch.allocate_fast(m_pipelines.size() * sizeof(QueuedDraw), alignof(QueuedDraw)));
		auto* casters = static_cast<BucketDraw*>(
			frame.scratch.allocate_fast(m_pipelines.size() * sizeof(BucketDraw), alignof(BucketDraw)));
		u32 count		 = 0;
		u32 caster_count = 0;

		for (u32 bucket = 0; bucket < m_pipelines.size() && bucket < frame.buckets.ranges.size(); ++bucket)
		{
			const BucketPipeline& entry = m_pipelines[bucket];
			const BucketRange range		= frame.buckets.ranges[bucket];
			const material::Type* type	= frame.materials.type(entry.type);

			if (range.capacity == 0 || type == nullptr)
				continue;

			const u32 table = frame.materials.table_index(entry.type);

			if (!entry.pipeline.is_null())
			{
				queued[count++] = {
					.draw	  = {entry.pipeline, table, bucket, range},
					.queue	  = type->state.queue,
					.priority = type->state.priority,
				};
			}

			if (!entry.shadow.is_null())
				casters[caster_count++] = {entry.shadow, table, bucket, range};
		}

		std::sort(
			queued, queued + count, [](const QueuedDraw& a, const QueuedDraw& b)
			{ return std::tie(a.queue, a.priority, a.draw.bucket) < std::tie(b.queue, b.priority, b.draw.bucket); });

		auto* draws =
			static_cast<BucketDraw*>(frame.scratch.allocate_fast(count * sizeof(BucketDraw), alignof(BucketDraw)));
		u32 solid = 0;

		for (u32 i = 0; i < count; ++i)
		{
			draws[i] = queued[i].draw;
			solid += queued[i].queue != material::Queue::Transparent ? 1 : 0;
		}

		const GraphTexture target =
			frame.resources.scene_color.is_null() ? frame.resources.output : frame.resources.scene_color;

		frame.resources.scene_depth = frame.graph.create({
			.name	= "scene_depth",
			.format = m_depth_format,
			.usage	= gpu::TextureUsage::DepthStencilTarget,
			.extent = frame.resources.scene_extent,
		});

		// A pass's recording: what every bucket shares, then each bucket's pipeline, table and draws.
		// Everything is captured by value into the frame's memory, which outlives the recording.
		const auto record =
			[constants = frame.constants, index_buffer = frame.geometry.index_buffer()](
				const ViewVisibility& visibility, const ViewConstants& view, const BucketDraw* first, u32 draw_count)
		{
			return [=](gpu::CommandList& cmd, const PassContext& ctx)
			{
				bind_scene_constants(cmd, constants, view);
				cmd.set_index_buffer(index_buffer);

				for (u32 i = 0; i < draw_count; ++i)
				{
					cmd.set_pipeline(first[i].pipeline);
					cmd.set_push_constants(DrawConstants{first[i].materials});
					draw_bucket(cmd, ctx, visibility, first[i].bucket, first[i].range);
				}
			};
		};

		// Shadow maps first, so every pass after may sample them. Each clears to the far plane even
		// with nothing to cast, so a map is never read before this frame wrote it.
		for (u32 i = 0; i < frame.resources.shadow_map_count; ++i)
		{
			const ShadowMap& shadow = frame.resources.shadow_maps[i];

			RenderGraph::Pass& shadow_pass = frame.graph.pass("surface.shadow").depth({.texture = shadow.texture});

			read(shadow_pass, frame.visibility[shadow.view]);
			shadow_pass.record(
				record(frame.visibility[shadow.view], frame.view_constants[shadow.view], casters, caster_count));
		}

		const auto read_shadows = [&frame](RenderGraph::Pass& pass)
		{
			for (u32 i = 0; i < frame.resources.shadow_map_count; ++i)
				pass.read(frame.resources.shadow_maps[i].texture);
		};

		// Opaque and cutout types write colour and depth. The pass clears both even with nothing to
		// draw, so the targets later features read always hold this frame.
		RenderGraph::Pass& solid_pass =
			frame.graph.pass("surface")
				.color({.texture = target, .clear = m_clear})
				.depth({.texture = frame.resources.scene_depth, .store = gpu::StoreOp::Store});

		read(solid_pass, frame.visibility[0]);
		read_shadows(solid_pass);
		solid_pass.record(record(frame.visibility[0], frame.view_constants[0], draws, solid));

		if (solid == count)
			return;

		// Transparent types blend over the finished world, testing its depth.
		RenderGraph::Pass& blended_pass =
			frame.graph.pass("surface.transparent")
				.color({.texture = target, .load = gpu::LoadOp::Load})
				.depth(
					{.texture = frame.resources.scene_depth, .load = gpu::LoadOp::Load, .store = gpu::StoreOp::Store});

		read(blended_pass, frame.visibility[0]);
		read_shadows(blended_pass);
		blended_pass.record(record(frame.visibility[0], frame.view_constants[0], draws + solid, count - solid));
	}

	GraphicsPipelineHandle SurfaceFeature::build(gpu::Device& device, const material::Type& type) const noexcept
	{
		const material::State& state = type.state;

		// The type's say (cull, blend, depth writes) with the pass's: its targets and the reverse-Z
		// depth test every surface shares.
		return device.create_graphics_pipeline({
			.name		   = type.name.c_str(),
			.vertex		   = {.code = type.bytecode(), .entry = material::VERTEX_ENTRY},
			.fragment	   = {.code = type.bytecode(), .entry = material::COLOR_ENTRY},
			.color_formats = {m_color_format},
			.depth_format  = m_depth_format,
			.depth_test	   = true,
			.depth_write   = state.depth_write,
			.cull		   = state.cull,
			.blend		   = state.blend,
		});
	}

	void SurfaceFeature::destroy(gpu::Device& device, BucketPipeline& entry) noexcept
	{
		const BucketPipeline gone = std::exchange(entry, {});
		device.destroy(gone.pipeline);
		device.destroy(gone.shadow);
	}

	GraphicsPipelineHandle SurfaceFeature::build_shadow(gpu::Device& device, const material::Type& type) const noexcept
	{
		const material::State& state = type.state;

		// Transparent types let the light through, and a type may say it casts nothing.
		if (!state.casts_shadow || state.queue == material::Queue::Transparent)
			return {};

		// Depth only, from both faces: a card has one side, and whichever faces the light casts. The
		// depth entry runs surface() for its discard, which gives a cutout's shadow its silhouette; a
		// type that never discards compiles it to nothing. Depth clamps rather than clips, for casters
		// between the light and the map's near plane.
		return device.create_graphics_pipeline({
			.name		  = type.name.c_str(),
			.vertex		  = {.code = type.bytecode(), .entry = material::VERTEX_ENTRY},
			.fragment	  = {.code = type.bytecode(), .entry = material::DEPTH_ENTRY},
			.color_count  = 0,
			.depth_format = SHADOW_MAP_FORMAT,
			.depth_test	  = true,
			.depth_write  = true,
			.depth_bias	  = m_shadow_bias,
			.depth_clamp  = true,
			.cull		  = gpu::CullMode::None,
		});
	}
}
