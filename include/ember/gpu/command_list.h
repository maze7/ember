#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/buffer.h>
#include <ember/gpu/common.h>

#include <type_traits>
#include <utility>

namespace ember::gpu
{
	struct Backend;
	struct Recording;

	/**
	 * Where a texture's memory sits for the next stretch of GPU work. One state names a
	 * (stage, access, layout) row the backend translates.
	 */
	enum class TextureState : u8
	{
		Undefined,	  // contents garbage; only legal as `before` (fresh images, swapchain acquires)
		RenderTarget, //
		DepthTarget,  //
		ShaderRead,	  //
		ShaderWrite,  // storage image access, any shader stage
		CopySrc,	  //
		CopyDst,	  //
		Present,	  // only legal as `after`; the presentation engine takes over from here
	};

	struct TextureBarrier
	{
		TextureHandle texture{};
		TextureState before = TextureState::Undefined;
		TextureState after	= TextureState::Undefined;

		/// Whole image by default. Narrow when levels move independently, the
		/// mip pyramid downsample being the canonical case.
		u32 base_mip	= 0;
		u32 mip_count	= ALL_MIPS;
		u32 base_layer	= 0;
		u32 layer_count = ALL_LAYERS;
	};

	/**
	 * Where a buffer's bytes sit for the next stretch of GPU work. Buffers have no
	 * layouts, so a state is purely the (stage, access) scope to synchronize against;
	 * on first use an `before` is safe because there is nothing earlier to wait for.
	 */
	enum class BufferState : u8
	{
		ShaderRead,
		ShaderWrite,
		IndexBuffer,
		IndirectArgument,
		CopySrc,
		CopyDst,
	};

	struct BufferBarrier
	{
		BufferHandle buffer{};
		BufferState before = BufferState::ShaderRead;
		BufferState after  = BufferState::ShaderRead;
	};

	enum class LoadOp : u8
	{
		Load,
		Clear,
		DontCare,
	};

	enum class StoreOp : u8
	{
		Store,
		DontCare,
	};

	struct ClearColor
	{
		f32 r = 0.0f;
		f32 g = 0.0f;
		f32 b = 0.0f;
		f32 a = 1.0f;
	};

	struct ColorAttachment
	{
		TextureHandle texture{};
		u32 mip		  = 0;
		u32 layer	  = 0;
		LoadOp load	  = LoadOp::Clear;
		StoreOp store = StoreOp::Store;
		ClearColor clear{};
	};

	struct DepthAttachment
	{
		TextureHandle texture{}; // null = no depth
		u32 mip			= 0;
		u32 layer		= 0;
		LoadOp load		= LoadOp::Clear;
		StoreOp store	= StoreOp::Store;
		f32 clear_depth = 0.0f; // reverse Z: zero is the far plane
	};

	struct RenderingDef
	{
		Span<const ColorAttachment> colors = {};
		DepthAttachment depth{};
	};

	/// Bit-identical to VkDrawIndirectCommand and D3D12_DRAW_ARGUMENTS: one buffer layout
	/// serves every backend and can be baked into cooked data.
	struct DrawIndirectArgs
	{
		u32 vertex_count   = 0;
		u32 instance_count = 0;
		u32 first_vertex   = 0;
		u32 first_instance = 0;
	};
	static_assert(sizeof(DrawIndirectArgs) == 16);

	/// Bit-identical to VkDrawIndexedIndirectCommand and D3D12_DRAW_INDEXED_ARGUMENTS.
	struct DrawIndexedIndirectArgs
	{
		u32 index_count	   = 0;
		u32 instance_count = 0;
		u32 first_index	   = 0;
		i32 base_vertex	   = 0;
		u32 first_instance = 0;
	};
	static_assert(sizeof(DrawIndexedIndirectArgs) == 20);

	/// Bit-identical to VkDispatchIndirectCommand and D3D12_DISPATCH_ARGUMENTS.
	struct DispatchIndirectArgs
	{
		u32 x = 1;
		u32 y = 1;
		u32 z = 1;
	};
	static_assert(sizeof(DispatchIndirectArgs) == 12);

	/**
	 * Records GPU work inside a frame. Device::begin_command_list() hands one out,
	 * Device::submit() seals it; the render area, viewport and scissor come from the attachments,
	 * flipped so clip space is Y up on every backend.
	 *
	 * Move-only: submit() consumes the list, so no copy can keep recording into a
	 * sealed command buffer. A default-constructed, moved-from or submitted list
	 * ignores every call.
	 */
	class CommandList
	{
	public:
		CommandList() = default;

		CommandList(const CommandList&)			   = delete;
		CommandList& operator=(const CommandList&) = delete;

		CommandList(CommandList&& other) noexcept
			: m_backend(std::exchange(other.m_backend, nullptr)), m_recording(std::exchange(other.m_recording, nullptr))
		{
		}

		CommandList& operator=(CommandList&& other) noexcept
		{
			m_backend	= std::exchange(other.m_backend, nullptr);
			m_recording = std::exchange(other.m_recording, nullptr);
			return *this;
		}

		void barrier(Span<const TextureBarrier> barriers, Span<const BufferBarrier> buffers = {}) noexcept;
		void barrier(const TextureBarrier& single) noexcept { barrier({&single, 1}); }
		void barrier(const BufferBarrier& single) noexcept { barrier({}, {&single, 1}); }
		void memory_barrier() noexcept;

		void begin_rendering(const RenderingDef& def) noexcept;
		void end_rendering() noexcept;

		void set_viewport(const Viewport& viewport) noexcept;
		void set_scissor(const Rect2D& scissor) noexcept;

		void set_pipeline(GraphicsPipelineHandle pipeline) noexcept;
		void set_pipeline(ComputePipelineHandle pipeline) noexcept;

		template <typename T> void set_push_constants(const T& data) noexcept
		{
			static_assert(std::is_trivially_copyable_v<T>, "push constants are raw bytes");
			static_assert(sizeof(T) <= PUSH_CONSTANT_BYTES, "push block exceeds the contract");
			push(&data, sizeof(T));
		}

		/**
		 * Fills constant slot `slot` for the draws and dispatches that follow: allocates
		 * transient space, copies `data`, rebinds the slot's dynamic offset at the next
		 * draw or dispatch. Slots persist until overwritten.
		 *
		 * Slot use by frequency is the convention: 0 per frame, 1 per pass, 2 per draw.
		 */
		template <typename T> void set_constants(u32 slot, const T& data) noexcept
		{
			static_assert(std::is_trivially_copyable_v<T>, "constants are raw bytes");
			static_assert(sizeof(T) <= 16384, "constant blocks stay within the spec floor every adapter guarantees");
			set_constants_raw(slot, &data, sizeof(T));
		}

		void set_index_buffer(BufferHandle buffer, IndexFormat format = IndexFormat::U32, u64 offset = 0) noexcept;

		void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0, u32 first_instance = 0) noexcept;
		void draw_indexed(
			u32 index_count,
			u32 instance_count = 1,
			u32 first_index	   = 0,
			i32 base_vertex	   = 0,
			u32 first_instance = 0) noexcept;

		/// Records are tightly packed unless `stride` says otherwise. A wider stride carries
		/// per draw payload beside the args.
		void
		draw_indirect(BufferHandle args, u64 offset, u32 draw_count, u32 stride = sizeof(DrawIndirectArgs)) noexcept;
		void draw_indexed_indirect(
			BufferHandle args, u64 offset, u32 draw_count, u32 stride = sizeof(DrawIndexedIndirectArgs)) noexcept;

		/// GPU decides the draw count; `max_draw_count` bounds what the args buffer
		/// can hold. Requires caps.indirect_count.
		void draw_indirect_count(
			BufferHandle args,
			u64 offset,
			BufferHandle count,
			u64 count_offset,
			u32 max_draw_count,
			u32 stride = sizeof(DrawIndirectArgs)) noexcept;
		void draw_indexed_indirect_count(
			BufferHandle args,
			u64 offset,
			BufferHandle count,
			u64 count_offset,
			u32 max_draw_count,
			u32 stride = sizeof(DrawIndexedIndirectArgs)) noexcept;

		// Compute and copies record outside render passes.
		void dispatch(u32 x, u32 y = 1, u32 z = 1) noexcept;
		void dispatch_indirect(BufferHandle args, u64 offset) noexcept;

		void copy_buffer(BufferHandle src, u64 src_offset, BufferHandle dst, u64 dst_offset, u64 size) noexcept;

		/// Offset and size are multiples of 4 (fill writes whole u32 words).
		void fill_buffer(BufferHandle dst, u64 offset, u64 size, u32 value) noexcept;

		/// Opens a debug label (RenderDoc, Nsight) and a GPU timestamp pair. Zones nest;
		/// every begin needs its end before submit. `name` must be a string literal or
		/// otherwise outlive the frame.
		void begin_zone(const char* name, u32 color = 0) noexcept;
		void end_zone() noexcept;

	private:
		friend class Device;

		void push(const void* data, u32 size) noexcept;
		void set_constants_raw(u32 slot, const void* data, u32 size) noexcept;

		Backend* m_backend	   = nullptr;
		Recording* m_recording = nullptr;
	};

	class [[nodiscard]] GpuZoneScope
	{
	public:
		GpuZoneScope(CommandList& list, const char* name, u32 color = 0) noexcept : m_list(&list)
		{
			list.begin_zone(name, color);
		}

		~GpuZoneScope() noexcept { m_list->end_zone(); }

		GpuZoneScope(const GpuZoneScope&)			 = delete;
		GpuZoneScope& operator=(const GpuZoneScope&) = delete;

	private:
		CommandList* m_list;
	};

#define EMBER_GPU_ZONE_JOIN(a, b) a##b
#define EMBER_GPU_ZONE_NAME(line) EMBER_GPU_ZONE_JOIN(ember_gpu_zone_, line)
#define EMBER_GPU_ZONE(list, ...) ::ember::gpu::GpuZoneScope EMBER_GPU_ZONE_NAME(__LINE__)(list, __VA_ARGS__)
}
