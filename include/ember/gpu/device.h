#pragma once

#include <ember/gpu/transient.h>
#include <ember/core/common.h>
#include <ember/gpu/buffer.h>
#include <ember/gpu/common.h>
#include <ember/gpu/swapchain.h>

// Forward declarations.
namespace ember
{
	class Platform;
}

namespace ember::gpu
{
	enum class AdapterPreference : u8
	{
		Discrete,	// prefer a dGPU, fall back to anything (default)
		Integrated, // prefer the iGPU (power, or a laptop whose dGPU can't present)
		Any			// first adapter that satisfies the feature set
	};

	/// Mirrors VkPhysicalDeviceType / DXGI adapter flags; ordering used for scoring.
	enum class AdapterKind : u8
	{
		Other,
		Integrated,
		Discrete,
		Virtual,
		Cpu, // software rasterizer (lavapipe, WARP): last resort, but allows for CI
		Count,
	};

	/**
	 * Resource pool capacities. Fixed at device creation because a handle's index
	 * is also its bindless slot (see gpu/common.h).
	 *
	 * Each must be <= 65535 (32 bit handles).
	 *
	 * Samplers additionally must be <= MAX_BINDLESS_SAMPLERS.
	 */
	struct DeviceLimits
	{
		u32 max_buffers					= 16384;
		u32 max_textures				= 16384; // views included: a view is a texture handle
		u32 max_samplers				= 1024;
		u32 max_graphics_pipelines		= 2048;
		u32 max_compute_pipelines		= 512;
		u32 max_swapchains				= 4;
		u32 max_recording_threads		= 8; // threads that may call begin_command_list()
		u32 max_command_lists_per_frame = 32;
	};

	struct DeviceDef
	{
		/// Window-system glue (surfaces, instance extensions).
		Platform* platform = nullptr;

		const char* app_name = "Ember";

		/// Validation layers + debug messenger. Off in release builds unless forced.
		bool enable_validation = GPU_VALIDATION_DEFAULT;

		/// Synchronization validation is an orderr of magnitude slower; opt in when hunting hazards.
		bool enable_sync_validation = false;

		/// EMBER_ASSERT inside the messenger so the debugger stops at the offending call
		bool break_on_validation_error = false;

		/// Optional override for gpu adapter
		AdapterPreference adapter = AdapterPreference::Discrete;

		/// CPU/GPU overlap depth. 2 or 3 is the sweet spot.
		u32 frames_in_flight = 2;

		/// Per-frame bumo ring for constants/instances (per slot; total = frames_in_flight * this)
		u64 transient_ring_bytes = 32_mb;

		/// Staging ring for update_buffer/update_texture/initial_data (per slot)
		u64 staging_ring_bytes = 64_mb;

		/// Resource pool limits
		DeviceLimits limits{};

		/// Pipeline cache blob path (loaded at boot when valid for this adapter, saved at shutdown).
		/// nullptr = in-memory cache only.
		const char* pipeline_cache_path = nullptr;
	};

	/**
	 * Adapter capabilities and the alignments user code must honour.
	 *
	 * Filled once at device boot; everything the layer needs at runtime is copied into the backend,
	 * so reading caps is never on a hot path.
	 */
	struct DeviceCaps
	{
		char adapter_name[128]	 = {};
		u32 vendor_id			 = 0;
		u32 device_id			 = 0;
		u32 api_version			 = 0; // backend-packed
		AdapterKind adapter_kind = AdapterKind::Other;

		u64 device_local_bytes		   = 0;
		bool host_visible_device_local = false; // ReBAR / SAM: the transient ring lives in VRAM

		u32 constant_buffer_offset_alignment = 256; // set_constant_buffer offsets are multiples of this
		u32 storage_buffer_offset_alignment	 = 256;
		u32 max_constant_block_bytes		 = 16384; // largest constant block bindable through a slot
		u32 copy_row_pitch_alignment		 = 1;	  // buffer<->texture copies (D3D12: 256)
		u32 copy_offset_alignment			 = 1;	  // (D3D12: 512)

		u32 max_texture_2d		  = 0;
		u32 max_texture_3d		  = 0;
		u32 max_texture_layers	  = 0;
		u32 max_color_attachments = 0;
		u32 max_anisotropy		  = 1;
		u32 subgroup_size		  = 0;

		f32 timestamp_period_ns = 0.0f; // ticks -> ns for GPU zones
		bool timestamps			= false;

		bool wireframe		= false; // FillMode::Wireframe
		bool indirect_count = false; // draw_*_indirect_count
		bool mesh_shaders	= false;
		bool sampler_minmax = false; // ReductionMode::Min/Max (Hi-Z)
		bool ray_tracing	= false; // reserved
		bool buffer_device_address = false;
		bool memory_budget = false;
	};

	struct FrameInfo
	{
		u32 frame_index = 0; // monotonically increasing
		u32 slot		= 0; // frame_index % frames_in_flight
	};

	struct Backend;

	/**
	 * The GPU device: owns every GPU object, the frame loop and the bindless heap.
	 *
	 * BACKEND
	 *   Exactly one backend is compiled in (EMBER_GPU_VULKAN / EMBER_GPU_NULL). The public
	 *   methods below are implemented directly by that backend; `Impl` is its private state.
	 *   No virtual dispatch, no forwarding layer.
	 *
	 * OWNERSHIP & LIFETIME
	 *   Stack-local subsystem, constructed after Platform and destroyed before it. Resources
	 *   are handles into fixed-capacity pools; destroy() invalidates the handle immediately
	 *   and releases the native object once every frame that could reference it has retired.
	 *   All resources must be destroyed before the device; survivors are reported (debug) and
	 *   released (release) at shutdown.
	 *
	 * FRAME MODEL
	 *   begin_frame() -> acquire() per swapchain -> record CommandLists -> submit() -> end_frame().
	 *   begin_frame waits for the frame that used the same slot frames_in_flight ago; that single
	 *   wait is what makes command pools, transient memory and deferred deletions safe to reuse.
	 *
	 * THREADING
	 *   Owner thread (the constructing thread): lifecycle, create/destroy/update, begin/end_frame,
	 *   acquire, submit, wait_idle. Any thread: CommandList recording (one thread per list from
	 *   begin to submit) and allocate_transient. Violations assert in debug builds.
	 *
	 * FAILURE MODEL
	 *   No exceptions. Resource creation returns a null handle and logs; the constructor logs
	 *   and leaves the device falsy. Device loss is sticky (device_lost()).
	 */
	class Device final
	{
	public:
		Device(const DeviceDef& def) noexcept;
		~Device() noexcept;

		Device(const Device&)			 = delete;
		Device& operator=(const Device&) = delete;
		Device(Device&&)				 = delete;
		Device& operator=(Device&&)		 = delete;

		[[nodiscard]] explicit operator bool() const noexcept { return m_state != nullptr; }

		[[nodiscard]] const DeviceCaps& caps() const noexcept;
		[[nodiscard]] bool device_lost() const noexcept;

		// [[nodiscard]] TextureHandle create_texture(const TextureDef&& def) noexcept;
		// [[nodiscard]] SamplerHandle create_sampler(const SamplerDef&& def) noexcept;
		// [[nodiscard]] GraphicsPipelineHandle create_graphics_pipeline(const GraphicsPipelineDef&& def) noexcept;
		// [[nodiscard]] ComputePipelineHandle create_compute_pipeline(const ComputePipelineDef&& def) noexcept;
		[[nodiscard]] SwapchainHandle create_swapchain(const SwapchainDef& def) noexcept;
		[[nodiscard]] BufferHandle create_buffer(const BufferDef& def) noexcept;

		[[nodiscard]] bool is_valid(BufferHandle handle) const noexcept;
		// [[nodsicard]] bool is_valid(TextureHandle handle) noexcept;
		// [[nodiscard]] bool is_valid(SamplerHandle handle) noexcept;
		// [[nodiscard]] bool is_valid(GraphicsPipelineHandle handle) noexcept;
		// [[nodiscard]] bool is_valid(ComputePipelineHandle handle) noexcept;
		// [[nodiscard]] bool is_valid(SwapchainHandle handle) const noexcept;

		void destroy(BufferHandle handle) noexcept;
		// void destroy(TextureHandle handle) noexcept;
		// void destroy(SamplerHandle handle) noexcept;
		// void destroy(GraphicsPipelineHandle handle) noexcept;
		// void destroy(ComputePipelineHandle handle) noexcept;
		void destroy(SwapchainHandle handle) noexcept;

		[[nodiscard]] TextureHandle acquire(SwapchainHandle handle) noexcept;
		[[nodiscard]] Extent2D swapchain_extent(SwapchainHandle handle) const noexcept;

		[[nodiscard]] void* mapped(BufferHandle handle) noexcept;

		/**
		 * The per-frame transient allocator. Allocations are valid from begin_frame to the
		 * retirement of the frame that made them; between frames every allocation reports
		 * invalid (the allocator is poisoned). Callable from any thread.
		 * The memory is write-combined: write sequentially, never read.
		 */
		[[nodiscard]] TransientAllocator& transient() noexcept;

		/**
		 * Schedules a copy into `handle` at `offset`.
		 *
		 * DeviceLocal: staged through the ring; the copy lands before this frame's GPU work
		 * (or before the next frame's, when called outside begin/end_frame, wait_idle also
		 * flushes). Same-frame updates to overlapping ranges of one buffer are unordered.
		 *
		 * Upload/Readback: written straight through the persistent mapping; hazards against
		 * in-flight GPU reads are the caller's to avoid (version per frame, or use transient).
		 */
		void update_buffer(BufferHandle handle, u64 offset, Span<const u8> data) noexcept;

		FrameInfo begin_frame() noexcept;
		// void submit(CommandList& list) noexcept;
		// void submit(Span<CommandList* const> lists) noexcept;
		void end_frame() noexcept;

		/// Blocks until the GPU is idle and drains every deferred deletion.l Never call it per frame.
		void wait_idle() noexcept;

		/// Validation messages seen since construction. Tests assert both are 0 after teardown.
		[[nodiscard]] static u32 validation_error_count() noexcept;
		[[nodiscard]] static u32 validation_warning_count() noexcept;

	private:
		void destroy_resources() noexcept;
		void shutdown() noexcept;

		/// Compile-time polymorphic platform-specific state (Vulkan, DX12, etc.)
		Backend* m_state = nullptr;
	};

	namespace detail
	{
		[[nodiscard]] constexpr bool valid_capacity(u32 value, u32 maximum) noexcept
		{
			return value != 0 && value <= maximum;
		}
	}

	[[nodiscard]] constexpr bool is_valid(const DeviceLimits& limits) noexcept
	{
		constexpr u32 MAX_POOL_CAPACITY = std::numeric_limits<u16>::max();

		const u32 general_pools[] = {
			limits.max_buffers,
			limits.max_textures,
			limits.max_graphics_pipelines,
			limits.max_compute_pipelines,
		};

		for (const u32 capacity : general_pools)
		{
			if (!detail::valid_capacity(capacity, MAX_POOL_CAPACITY))
			{
				return false;
			}
		}

		if (!detail::valid_capacity(limits.max_samplers, MAX_BINDLESS_SAMPLERS))
		{
			return false;
		}

		if (!detail::valid_capacity(limits.max_swapchains, MAX_SWAPCHAINS))
		{
			return false;
		}

		if (limits.max_recording_threads == 0)
			return false;

		if (limits.max_command_lists_per_frame == 0)
			return false;

		return true;
	}

	[[nodiscard]] constexpr bool is_valid(const DeviceDef& def) noexcept
	{
		if (def.app_name == nullptr)
			return false;

		if (def.frames_in_flight == 0 || def.frames_in_flight > MAX_FRAMES_IN_FLIGHT)
		{
			return false;
		}

		if (def.transient_ring_bytes == 0 || def.staging_ring_bytes == 0)
		{
			return false;
		}

		if (!def.enable_validation && (def.enable_sync_validation || def.break_on_validation_error))
		{
			return false;
		}

		if (!is_valid(def.limits))
			return false;

		switch (def.adapter)
		{
			case AdapterPreference::Discrete:
			case AdapterPreference::Integrated:
			case AdapterPreference::Any:
				return true;
		}

		return false;
	}
}

namespace ember
{
	EMBER_ENUM_NAMES(gpu::AdapterKind, "Other", "Integrated", "Discrete", "Virtual", "Cpu");
}
