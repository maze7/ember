#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/buffer.h>
#include <ember/gpu/command_list.h>
#include <ember/gpu/common.h>
#include <ember/render/frame_constants.h>
#include <ember/render/graph.h>
#include <ember/render/material_registry.h>
#include <ember/render/view.h>

namespace ember::gpu
{
	class Device;
}

namespace ember::render
{
	/**
	 * The frame's draw buckets: each bucket's range in every view's argument buffer, on the CPU for
	 * the passes that draw them and in the frame's slice of the transient ring for the cull.
	 */
	struct DrawBuckets
	{
		Span<const BucketRange> ranges = {}; // one per bucket, in the frame's scratch
		u32 table					   = 0;	 // bindless index of the transient buffer holding the same ranges
		u32 first					   = 0;	 // the element of bucket 0's range in it
		u32 draws					   = 0;	 // every range together
	};

	/**
	 * One view's GPU-produced draws: the cull wrote each visible object into its bucket's range of
	 * the argument buffer and counted it. Raster passes consume them without knowing how they were
	 * produced.
	 */
	struct ViewVisibility
	{
		GraphBuffer args   = {};	// DrawIndexedIndirectArgs, each bucket's from its range's first entry
		GraphBuffer counts = {};	// one u32 per bucket
		bool use_count	   = false; // caps.indirect_count; without it every range was cleared to empty draws
	};

	/// Declares a pass's consumption of a view's draws. The fallback draw never reads the counts, so
	/// they are only an indirect argument when consumed.
	inline void read(RenderGraph::Pass& pass, const ViewVisibility& visibility) noexcept
	{
		pass.read(visibility.args, gpu::BufferState::IndirectArgument);

		if (visibility.use_count)
			pass.read(visibility.counts, gpu::BufferState::IndirectArgument);
	}

	/// Issues one bucket's draws: one indirect multi-draw over its range. The caller binds the
	/// bucket's pipeline, the constants and the shared index buffer first.
	inline void draw_bucket(gpu::CommandList& cmd, const PassContext& ctx, const ViewVisibility& visibility, u32 bucket,
							BucketRange range) noexcept
	{
		if (range.capacity == 0)
			return;

		const u64 offset = u64{range.first} * sizeof(gpu::DrawIndexedIndirectArgs);

		if (visibility.use_count)
		{
			cmd.draw_indexed_indirect_count(ctx.buffer(visibility.args), offset, ctx.buffer(visibility.counts),
											u64{bucket} * sizeof(u32), range.capacity);
		}
		else
		{
			// The clear pass zeroed every range, so entries past the cull's count cost only
			// command processing.
			cmd.draw_indexed_indirect(ctx.buffer(visibility.args), offset, range.capacity);
		}
	}

	struct VisibilityDef
	{
		Span<const u8> cull_shader = {}; // cooked blob, entry cs_main

		/// Entries in a view's argument buffer: every object's draw, whichever bucket it lands in.
		u32 command_capacity = 1u << 17;

		/// Counters in a view's count buffer: the registry's bucket_count().
		u32 bucket_capacity = 256;
	};

	[[nodiscard]] constexpr bool is_valid(const VisibilityDef& def) noexcept
	{
		return !def.cull_shader.empty() && def.command_capacity != 0 && def.bucket_capacity != 0;
	}

	/**
	 * GPU frustum culling: one dispatch per view walks the object table and writes each survivor's
	 * draw into the range its material's bucket owns, keyed by the object slot riding
	 * first_instance. The ranges are laid out on the CPU from how many objects use each material,
	 * so one pass sorts any number of objects into any number of buckets, with one atomic each and
	 * no second pass to compact them.
	 *
	 * The scene tables are read bindlessly and stay outside the graph; the staging batch barriers
	 * already order every sync write before this frame's dispatches. The argument and count buffers
	 * are graph transients of fixed size, so the pool recycles them however the scene changes, and
	 * clear, cull write and indirect consumption all get derived barriers.
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

		/// Adds the clear and cull passes for one view and returns the draws its raster passes read.
		[[nodiscard]] ViewVisibility cull(RenderGraph& graph, const FrameConstants& constants,
										  const DrawBuckets& buckets, u32 slot_count, const View& view) noexcept;

	private:
		ComputePipelineHandle m_cull = {};

		u32 m_command_capacity	 = 0;
		u32 m_bucket_capacity	 = 0;
		u32 m_max_indirect_draws = 0; // adapter ceiling for submitted draw counts
		bool m_use_count		 = false;
	};
}
