#pragma once

#include <ember/containers/ring_buffer.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/input/input.h>
#include <ember/memory/pmr/arena.h>

#include <array>
#include <utility>

namespace ember
{
	/**
	 * One frame as the app sees it, handed to update() and then to render().
	 * A frame is a piece of data, not a length of time: everything a stage needs is in here
	 * or reachable from here, and nothing in here changes under a stage while it runs. Frames
	 * are numbered from 1 and the runtime keeps the last FrameRing::CAPACITY of them, so a stage
	 * can look back at what an earlier frame saw and measured through App::frame().
	 *
	 * The arenas are the frame's memory lifetimes, freed by tag, never per allocation. Each
	 * stage allocates only from th eone it owns:
	 *
	 *   sim_scratch     update() only.                     Gone once update() returns.
	 *   sim_to_render   update() writes, render() reads.   Gone once the frame has been rendered.
	 *   render_scratch  render() only.                     Gone once the frame has been submitted.
	 */
	struct FrameParams
	{
		u64 frame_index = 0;	// 1 for the first frame; 0 is boot, which no entry ever names
		u32 frame_slot	= 0;	// the device's frame in flight slot; per frame GPU resources index by it
		f32 dt			= 0.0f; // delta time since last frame

		/** The input devices as they stood when the frame began. Copied. */
		InputState input = {};

		TextureHandle backbuffer   = {}; // null when the frame has no drawable and render() is skipped.
		Extent2D backbuffer_extent = {};

		/**
		 * The frame's GPU work once end_frame() has handed it over; zero until then and for
		 * frames that never reached the GPU. App::is_frame_complete() reads it.
		 */
		gpu::FrameSubmission gpu = {};

		/**
		 * Stage timestamps in steady clock nanoseconds: compare them with each other, not the wall clock.
		 * A stage that did not run leaves zeros.
		 */
		u64 update_begin_ns = 0;
		u64 update_end_ns	= 0;
		u64 render_begin_ns = 0;
		u64 render_end_ns	= 0;

		Arena& sim_scratch;	   // MemoryLifetime::SimScratch
		Arena& sim_to_render;  // MemoryLifetime::SimToRender
		Arena& render_scratch; // MemoryLifetime::RenderScratch

		/// Milliseconds a stage took, zero while it runs or when it did not run.
		[[nodiscard]] f32 update_ms() const noexcept { return stage_ms(update_begin_ns, update_end_ns); }
		[[nodiscard]] f32 render_ms() const noexcept { return stage_ms(render_begin_ns, render_end_ns); }

	private:
		[[nodiscard]] static f32 stage_ms(u64 begin_ns, u64 end_ns) noexcept
		{
			return end_ns > begin_ns ? static_cast<f32>(end_ns - begin_ns) * 1.0e-6f : 0.0f;
		}
	};

	/**
	 * The last CAPACITY frames: a ring of slots reused in order, one begun per frame and handed
	 * to the stages, the ones behind it history complete with what each stage measured. Frames
	 * count from 1 and the ring from 0, so frame N is the ring's value N - 1. Nothing here is
	 * synchronized: the owner thread begins frames, and readers only ever look at frames that
	 * have already begun.
	 */
	class FrameRing final
	{
	public:
		static constexpr u32 CAPACITY = 16;

		/// Every slot names the same three arenas: allocators are per lifetime, tags are per frame.
		FrameRing(Arena& sim_scratch, Arena& sim_to_render, Arena& render_scratch) noexcept
			: m_frames(
				  [&](u32)
				  {
					  return FrameParams{
						  .sim_scratch	  = sim_scratch,
						  .sim_to_render  = sim_to_render,
						  .render_scratch = render_scratch,
					  };
				  })
		{
		}

		FrameRing(const FrameRing&)			   = delete;
		FrameRing& operator=(const FrameRing&) = delete;

		/**
		 * Claims the slot for frame index, one past the last begun (1 on a fresh ring), resets its
		 * per frame fields and copies input into it. The frame it evicts is index minus CAPACITY.
		 */
		FrameParams& begin(u64 index, f32 dt, const InputState& input) noexcept
		{
			EMBER_ASSERT(index == current_index() + 1 && "frames begin in order, from 1");

			FrameParams& frame = m_frames.push();

			frame.frame_index		= index;
			frame.frame_slot		= 0;
			frame.dt				= dt;
			frame.input				= input;
			frame.backbuffer		= {};
			frame.backbuffer_extent = {};
			frame.gpu				= {};
			frame.update_begin_ns	= 0;
			frame.update_end_ns		= 0;
			frame.render_begin_ns	= 0;
			frame.render_end_ns		= 0;

			return frame;
		}

		/// The frame index names; null once it has been evicted, before it has begun, and for 0.
		[[nodiscard]] const FrameParams* find(u64 index) const noexcept
		{
			return index == 0 ? nullptr : m_frames.find(index - 1);
		}

		/// The most recently begun frame, 0 before the first begin() and after clear().
		[[nodiscard]] u64 current_index() const noexcept { return m_frames.next_sequence(); }

		/// Forgets every frame, for a fresh run.
		void clear() noexcept { m_frames.clear(); }

	private:
		RingBuffer<FrameParams, CAPACITY> m_frames;
	};
}
