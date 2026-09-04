#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/render/visibility.h>

#include <cstring>

namespace ember::render
{
	namespace
	{
		/// Slot 1 block for the cull dispatch, mirrored in cull.slang. std140
		/// packs the trailing scalars tight after the vec4 array, so the C++
		/// layout matches byte for byte.
		struct CullConstants
		{
			glm::vec4 planes[6];
			u32 objects;
			u32 geometries;
			u32 slot_count;
			u32 layers;
		};
		static_assert(sizeof(CullConstants) == 112);

		struct CullPush
		{
			u32 args_opaque;
			u32 count_opaque;
			u32 args_cutout;
			u32 count_cutout;
		};

		[[nodiscard]] constexpr u64 slot_count_bytes(u32 draws) noexcept
		{
			return u64{draws} * sizeof(gpu::DrawIndexedIndirectArgs);
		}
	}

	void Visibility::init(gpu::Device& device, const VisibilityDef& def) noexcept
	{
		EMBER_ASSERT(m_cull.is_null() && "init runs once");
		EMBER_ASSERT(is_valid(def));

		m_command_capacity	 = def.command_capacity;
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

	ViewVisibility Visibility::cull(
		RenderGraph& graph, const GpuScene& gpu_scene, const GeometryPool& geometry, const View& view) noexcept
	{
		EMBER_ASSERT(!m_cull.is_null() && "cull before init");

		const u32 slot_count = gpu_scene.slot_count();

		EMBER_ASSERT(slot_count <= m_command_capacity && "args capacity must cover every slot");
		EMBER_ASSERT(slot_count <= m_max_indirect_draws && "adapter cannot consume this many indirect draws");

		// Two buckets keyed off the object cutout flag: solid geometry and
		// alpha-tested sprites. Each sizes its args at capacity so the graph pool
		// recycles buffers as the scene high water grows, and bounds its submit at
		// the shared slot count.
		const auto make_stream = [&](const char* args_name, const char* count_name) -> DrawStream
		{
			DrawStream stream;
			stream.args			  = graph.create({
				.name  = args_name,
				.size  = u64{m_command_capacity} * sizeof(gpu::DrawIndexedIndirectArgs),
				.usage = m_use_count
							 ? gpu::BufferUsage::Storage | gpu::BufferUsage::Indirect
							 : gpu::BufferUsage::Storage | gpu::BufferUsage::Indirect | gpu::BufferUsage::CopyDst,
			});
			stream.count		  = graph.create({
				.name  = count_name,
				.size  = sizeof(u32),
				.usage = gpu::BufferUsage::Storage | gpu::BufferUsage::Indirect | gpu::BufferUsage::CopySrc |
						 gpu::BufferUsage::CopyDst,
			});
			stream.max_draw_count = slot_count;
			stream.use_count	  = m_use_count;
			return stream;
		};

		ViewVisibility visibility;
		visibility.opaque = make_stream("cull.opaque.args", "cull.opaque.count");
		visibility.cutout = make_stream("cull.cutout.args", "cull.cutout.count");

		const DrawStream opaque = visibility.opaque;
		const DrawStream cutout = visibility.cutout;

		RenderGraph::Pass& clear = graph.pass("cull_clear");
		clear.write(opaque.count, gpu::BufferState::CopyDst);
		clear.write(cutout.count, gpu::BufferState::CopyDst);
		if (!m_use_count)
		{
			clear.write(opaque.args, gpu::BufferState::CopyDst);
			clear.write(cutout.args, gpu::BufferState::CopyDst);
		}

		clear.record(
			[opaque, cutout](gpu::CommandList& cmd, const PassContext& ctx)
			{
				cmd.fill_buffer(ctx.buffer(opaque.count), 0, sizeof(u32), 0);
				cmd.fill_buffer(ctx.buffer(cutout.count), 0, sizeof(u32), 0);

				// Without indirect_count the drawn range must read as empty draws
				// before compaction writes the head, in both buckets.
				if (!opaque.use_count && slot_count_bytes(opaque.max_draw_count) > 0)
				{
					const u64 bytes = slot_count_bytes(opaque.max_draw_count);
					cmd.fill_buffer(ctx.buffer(opaque.args), 0, bytes, 0);
					cmd.fill_buffer(ctx.buffer(cutout.args), 0, bytes, 0);
				}
			});

		CullConstants constants{
			.objects	= gpu_scene.objects_index(),
			.geometries = geometry.table_index(),
			.slot_count = slot_count,
			.layers		= view.layers,
		};
		std::memcpy(constants.planes, view.frustum.planes, sizeof(constants.planes));

		graph.pass("cull")
			.write(opaque.args)
			.write(opaque.count)
			.write(cutout.args)
			.write(cutout.count)
			.record(
				[this, opaque, cutout, constants, slot_count](gpu::CommandList& cmd, const PassContext& ctx)
				{
					cmd.set_pipeline(m_cull);
					cmd.set_constants(1, constants);
					cmd.set_push_constants(
						CullPush{
							ctx.bindless(opaque.args),
							ctx.bindless(opaque.count),
							ctx.bindless(cutout.args),
							ctx.bindless(cutout.count)});
					cmd.dispatch((slot_count + 63) / 64);
				});

		return visibility;
	}
	void VisibilityReadback::init(gpu::Device& device) noexcept
	{
		EMBER_ASSERT(m_buffer.is_null() && "init runs once");

		m_buffer = device.create_buffer({
			.name	= "visibility.readback",
			.size	= MAX_FRAMES_IN_FLIGHT * VISIBILITY_QUERY_SLOTS * sizeof(u32),
			.usage	= gpu::BufferUsage::CopyDst,
			.memory = gpu::MemoryLocation::Readback,
		});

		if (m_buffer.is_null())
		{
			EMBER_ERROR("visibility readback creation failed");
			return;
		}

		// Zero the ring so the first frames report zero instead of whatever the
		// allocation held.
		void* mapped = device.mapped(m_buffer);
		std::memset(mapped, 0, MAX_FRAMES_IN_FLIGHT * VISIBILITY_QUERY_SLOTS * sizeof(u32));

		m_values = static_cast<const u32*>(mapped);
	}

	void VisibilityReadback::shutdown(gpu::Device& device) noexcept
	{
		if (!m_buffer.is_null())
			device.destroy(m_buffer);

		m_buffer = {};
		m_values = nullptr;
	}

	void VisibilityReadback::capture(RenderGraph& graph, GraphBuffer count, u32 frame_slot, u32 query_slot) noexcept
	{
		EMBER_ASSERT(m_values != nullptr && "capture before init");
		EMBER_ASSERT(frame_slot < MAX_FRAMES_IN_FLIGHT && query_slot < VISIBILITY_QUERY_SLOTS);

		const u64 offset = u64{frame_slot * VISIBILITY_QUERY_SLOTS + query_slot} * sizeof(u32);

		graph.pass("visibility_readback")
			.read(count, gpu::BufferState::CopySrc)
			.record([this, count, offset](gpu::CommandList& cmd, const PassContext& ctx)
					{ cmd.copy_buffer(ctx.buffer(count), 0, m_buffer, offset, sizeof(u32)); });
	}
}
