#pragma once

#include <ember/gpu/transient.h>
#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <ember/memory/memory.h>

#include <atomic>
#include <mutex>
#include <vk_mem_alloc.h>

namespace ember::gpu
{
	struct Backend;
}

namespace ember::gpu::vk
{
	/// Overflow pages are hard-capped: a workload that needs more than this is a bug.
	inline constexpr u32 MAX_TRANSIENT_PAGES = 8;

	/**
	 * One overflow page. Pool-registered (users bind transient memory by handle), recycled
	 * across frames: a page retired by frame N returns to the free list once N's timeline
	 * value completes. Sustained overflow therefore allocates nothing in steady state.
	 */
	struct TransientPage
	{
		BufferHandle handle{};
		VmaAllocation allocation = VK_NULL_HANDLE; // for the non-coherent flush fallback
		u8* cpu					 = nullptr;
		u64 size				 = 0;
		u64 used				 = 0;
		bool coherent			 = true;
	};

	struct RetiredPage
	{
		TransientPage page;
		u64 value = 0; // recycle when the frame timeline passes this
	};

	/**
	 * Backend state for the per-frame transient allocator (the fast path lives in
	 * ember/gpu/transient.h). One buffer holds frames_in_flight slices; begin_frame binds
	 * the frame's slice and the same wait that recycles command pools proves the slice is
	 * writable again. No extra sync objects exist here on purpose.
	 *
	 * THREADING
	 * 	The ring fields after boot are read-only. The mutex guards everything overflow:
	 * 	active/free/frame/retired pages and the telemetry. The fast path is lock-free.
	 */
	struct TransientRing
	{
		BufferHandle handle{};
		VmaAllocation allocation = VK_NULL_HANDLE;
		u8* cpu					 = nullptr;
		u64 slice_bytes			 = 0; // per slot; slice k begins at k * slice_bytes
		u64 frame_begin			 = 0; // active slice begin, for used() telemetry
		u64 page_bytes			 = 0;
		bool coherent			 = true;

		std::mutex mutex;
		TransientPage active{}; // page currently being sub-allocated, if any
		Vector<TransientPage> free_pages{&memory::heap(MemoryTag::Graphics)};
		Vector<TransientPage> frame_pages{&memory::heap(MemoryTag::Graphics)}; // fille dthis frame
		Vector<RetiredPage> retired{&memory::heap(MemoryTag::Graphics)};	   // stamped, awaiting recycle
		u32 page_count	   = 0;
		u64 overflow_bytes = 0;		 // this frame, under the mutex
		std::atomic<u32> starved{0}; // allocations refused for want of a page
	};

	/// Creates the ring buffer and registers it in the pool. False fails the boot.
	[[nodiscard]] bool transient_boot(Backend& backend, u64 per_slot_bytes) noexcept;

	/// Raw teardown for shutdown: GPU idle, workers stopped. Destroys the ring and every
	/// overflow page and erases their pool entries, so the shutdown leak sweep reports
	/// only genuine user buffers. Tolerates partial boots.
	void transient_destroy(Backend& backend) noexcept;

	/// Recycles provably-retired pages, services starvation, binds the frame's slice.
	void transient_begin_frame(Backend& backend, u32 slot) noexcept;

	/// Flushes non-coherent memory, retires this frame's pages stamped with `value`
	/// (the value this frame's submit signals), poisons the allocator, logs telemetry.
	/// Must run before the submit: the submit is what publishes the written bytes.
	void transient_end_frame(Backend& backend, u64 value) noexcept;

	/// The cold path installed into TransientAllocator::bind. `context` is the Backend.
	[[nodiscard]] TransientAllocation transient_overflow(void* context, u32 size, u32 alignment) noexcept;
}
