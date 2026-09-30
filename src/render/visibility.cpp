#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/render/visibility.h>

#include <cstring>

namespace ember::render
{
	namespace
	{
		/// The cull dispatch's view, mirrored in cull.slang. std140 packs the trailing scalars tight
		/// after the vec4 array, so the C++ layout matches byte for byte.
		struct CullConstants
		{
			glm::vec4 planes[6] = {};
			u32 slot_count		= 0;
			u32 layers			= 0;
			u32 buckets			= 0;
			u32 first_bucket	= 0;
			u32 required		= 0; // ObjectFlags every drawn object carries
			u32 pad[3]			= {};
		};

		static_assert(sizeof(CullConstants) == 128);

		struct CullPush
		{
			u32 args;
			u32 counts;
		};
	}

	void Visibility::init(gpu::Device& device, const VisibilityDef& def) noexcept
	{
		EMBER_ASSERT(m_cull.is_null() && "init runs once");
		EMBER_ASSERT(is_valid(def));

		m_command_capacity	 = def.command_capacity;
		m_bucket_capacity	 = def.bucket_capacity;
		m_max_indirect_draws = device.caps().max_indirect_draw_count;
		m_use_count			 = device.caps().indirect_count;

		m_cull = device.create_compute_pipeline({
			.name	= "visibility.cull",
			.shader = {.code = def.cull_shader, .entry = "cs_main"},
		});

		if (m_cull.is_null())
			EMBER_ERROR("visibility cull pipeline creation failed");
	}

	void Visibility::shutdown(gpu::Device& device) noexcept
	{
		if (!m_cull.is_null())
			device.destroy(m_cull);

		m_cull = {};
	}

	ViewVisibility Visibility::cull(RenderGraph& graph, const FrameConstants& constants, const DrawBuckets& buckets,
									u32 slot_count, const View& view) noexcept
	{
		EMBER_ASSERT(!m_cull.is_null() && "cull before init");
		EMBER_ASSERT(buckets.draws <= m_command_capacity && "args capacity must cover every object");
		EMBER_ASSERT(buckets.ranges.size() <= m_bucket_capacity && "counts capacity must cover every bucket");
		EMBER_ASSERT(slot_count <= m_max_indirect_draws && "adapter cannot consume this many indirect draws");

		// Both buffers keep one size whatever the scene holds, so the graph's pool hands the same
		// ones back frame after frame instead of reallocating as objects come and go.
		const ViewVisibility visibility{
			.args	   = graph.create({
				.name  = "cull.args",
				.size  = u64{m_command_capacity} * sizeof(gpu::DrawIndexedIndirectArgs),
				.usage = gpu::BufferUsage::Storage | gpu::BufferUsage::Indirect | gpu::BufferUsage::CopyDst,
			}),
			.counts	   = graph.create({
				.name  = "cull.counts",
				.size  = u64{m_bucket_capacity} * sizeof(u32),
				.usage = gpu::BufferUsage::Storage | gpu::BufferUsage::Indirect | gpu::BufferUsage::CopySrc |
						 gpu::BufferUsage::CopyDst,
			}),
			.use_count = m_use_count,
		};

		const u32 bucket_bytes = static_cast<u32>(buckets.ranges.size() * sizeof(u32));
		const u64 draw_bytes   = u64{buckets.draws} * sizeof(gpu::DrawIndexedIndirectArgs);

		RenderGraph::Pass& clear = graph.pass("cull_clear");
		clear.write(visibility.counts, gpu::BufferState::CopyDst);
		if (!m_use_count)
			clear.write(visibility.args, gpu::BufferState::CopyDst);

		clear.record(
			[visibility, bucket_bytes, draw_bytes](gpu::CommandList& cmd, const PassContext& ctx)
			{
				if (bucket_bytes != 0)
					cmd.fill_buffer(ctx.buffer(visibility.counts), 0, bucket_bytes, 0);

				// Without indirect_count every range is drawn to its capacity, so the entries the
				// cull leaves unwritten must read as empty draws.
				if (!visibility.use_count && draw_bytes != 0)
					cmd.fill_buffer(ctx.buffer(visibility.args), 0, draw_bytes, 0);
			});

		CullConstants cull{
			.slot_count	  = slot_count,
			.layers		  = view.layers,
			.buckets	  = buckets.table,
			.first_bucket = buckets.first,
			.required	  = static_cast<u32>(view.required),
		};
		std::memcpy(cull.planes, view.frustum.planes, sizeof(cull.planes));

		graph.pass("cull")
			.write(visibility.args)
			.write(visibility.counts)
			.record(
				[this, visibility, constants, cull](gpu::CommandList& cmd, const PassContext& ctx)
				{
					cmd.set_pipeline(m_cull);
					cmd.set_constants(CONSTANTS_FRAME, constants);
					cmd.set_constants(CONSTANTS_PASS, cull);
					cmd.set_push_constants(CullPush{ctx.bindless(visibility.args), ctx.bindless(visibility.counts)});
					cmd.dispatch((cull.slot_count + 63) / 64);
				});

		return visibility;
	}
}
