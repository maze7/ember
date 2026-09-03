#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/buffer.h>
#include <ember/gpu/command_list.h>
#include <ember/gpu/common.h>
#include <ember/render/geometry.h>
#include <ember/render/gpu_scene.h>
#include <ember/render/graph.h>
#include <ember/render/view.h>

namespace ember::gpu
{
	class Device;
}

namespace ember::render
{
	/**
	 * One view's worth of GPU-produced draws: compacted indirect arguments and
	 * the count the cull accumulated. Raster passes consume a stream without
	 * knowing how it was produced.
	 */
	struct DrawStream
	{
		GraphBuffer args  = {}; // DrawIndexedIndirectArgs[capacity], compacted from the front
		GraphBuffer count = {}; // one u32

		/// The tightest bound the producer can emit this frame (the gpu scene's
		/// slot count), and what the draw submits. The args buffer's capacity is
		/// a producer detail.
		u32 max_draw_count = 0;

		bool use_count = false; // caps.indirect_count; without it the zeroed head draws empty
	};

	struct ViewVisibility
	{
		DrawStream opaque = {};
	};

	/// Declares a pass's consumption of a stream. The fallback draw never reads
	/// the count buffer, so it is only an indirect argument when consumed.
	inline void read(RenderGraph::Pass& pass, const DrawStream& stream) noexcept
	{
		pass.read(stream.args, gpu::BufferState::IndirectArgument);

		if (stream.use_count)
			pass.read(stream.count, gpu::BufferState::IndirectArgument);
	}

	/// Issues a stream's draws. The caller binds pipeline, constants and the
	/// shared index buffer first; record callbacks take the (cmd, ctx) form so
	/// the stream's graph buffers resolve.
	inline void draw_indexed_stream(gpu::CommandList& cmd, const PassContext& ctx, const DrawStream& stream) noexcept
	{
		if (stream.max_draw_count == 0)
			return;

		if (stream.use_count)
		{
			cmd.draw_indexed_indirect_count(
				ctx.buffer(stream.args), 0, ctx.buffer(stream.count), 0, stream.max_draw_count);
		}
		else
		{
			// The clear pass zeroed the drawn range, so entries past the
			// compacted head cost only command processing.
			cmd.draw_indexed_indirect(ctx.buffer(stream.args), 0, stream.max_draw_count);
		}
	}

	struct VisibilityDef
	{
		Span<const u8> cull_shader = {}; // cooked blob, entry cs_main
		u32 command_capacity	   = 1u << 17;
	};

	[[nodiscard]] constexpr bool is_valid(const VisibilityDef& def) noexcept
	{
		return !def.cull_shader.empty() && def.command_capacity != 0;
	}

	/**
	 * GPU frustum culling: one dispatch per view walks the object table and
	 * compacts survivors into indirect draw arguments, keyed by the object slot
	 * riding first_instance.
	 *
	 * The scene tables are read bindlessly and stay outside the graph; the
	 * staging batch barriers already order every sync write before this frame's
	 * dispatches. The streams are graph transients, so clear, cull write and
	 * indirect consumption all get derived barriers.
	 *
	 * Instrumentation lives in VisibilityReadback and is opt in per stream.
	 */
	class Visibility
	{
	public:
		Visibility() = default;

		Visibility(const Visibility&)			 = delete;
		Visibility& operator=(const Visibility&) = delete;

		/// Creates the cull pipeline. Runs once.
		void init(gpu::Device& device, const VisibilityDef& def) noexcept;

		/// Destroys it. Call before the device goes down.
		void shutdown(gpu::Device& device) noexcept;

		/// Adds the clear and cull passes for one view and returns the streams
		/// its raster consumers read.
		[[nodiscard]] ViewVisibility
		cull(RenderGraph& graph, const GpuScene& gpu_scene, const GeometryPool& geometry, const View& view) noexcept;

	private:
		ComputePipelineHandle m_cull = {};

		u32 m_command_capacity	 = 0;
		u32 m_max_indirect_draws = 0; // adapter ceiling for submitted draw counts
		bool m_use_count		 = false;
	};

	/// Query slots let several streams (main view, cascades) capture per frame.
	inline constexpr u32 VISIBILITY_QUERY_SLOTS = 8;

	/**
	 * Debug counter readback: copies a stream's count into a per frame slot
	 * ring the CPU reads frames_in_flight later. A statistic, never a rendering
	 * input. Call capture() after the stream's consumers; passes execute in
	 * declaration order, so the copy lands behind the draw.
	 */
	class VisibilityReadback
	{
	public:
		VisibilityReadback() = default;

		VisibilityReadback(const VisibilityReadback&)			 = delete;
		VisibilityReadback& operator=(const VisibilityReadback&) = delete;

		void init(gpu::Device& device) noexcept;
		void shutdown(gpu::Device& device) noexcept;

		void capture(RenderGraph& graph, GraphBuffer count, u32 frame_slot, u32 query_slot = 0) noexcept;

		/// The value captured in this slot pair frames_in_flight ago; zero
		/// until the first capture retires.
		[[nodiscard]] u32 value(u32 frame_slot, u32 query_slot = 0) const noexcept
		{
			EMBER_ASSERT(frame_slot < MAX_FRAMES_IN_FLIGHT && query_slot < VISIBILITY_QUERY_SLOTS);
			return m_values != nullptr ? m_values[frame_slot * VISIBILITY_QUERY_SLOTS + query_slot] : 0;
		}

	private:
		BufferHandle m_buffer = {};
		const u32* m_values	  = nullptr;
	};
}
