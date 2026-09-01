#pragma once

#include <ember/core/common.h>
#include <ember/gpu/buffer.h>
#include <ember/gpu/command_list.h>
#include <ember/gpu/common.h>
#include <ember/gpu/texture.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/arena_resource.h>

#include <type_traits>

namespace ember::gpu
{
	class Device;
}

namespace ember::render
{
	/// Contract limits. Raise when a consumer outgrows them.
	inline constexpr u32 MAX_GRAPH_PASSES	= 64;
	inline constexpr u32 MAX_GRAPH_TEXTURES = 64;
	inline constexpr u32 MAX_GRAPH_BUFFERS	= 64;
	inline constexpr u32 MAX_PASS_USES		= 15;
	inline constexpr u32 MAX_POOL_ENTRIES	= 128;
	inline constexpr u32 GRAPH_ARENA_BYTES	= 16_kb;

	/// Frames a pooled texture may sit unused before the pool destroys it.
	inline constexpr u32 POOL_IDLE_FRAMES = 60;

	/**
	 * Graph-local name for a texture used this frame. Minted by create() or import(),
	 * dead after execute(). Resolves to a TextureHandle only inside pass callbacks,
	 * through the PassContext.
	 */
	struct GraphTexture
	{
		u16 index = 0xFFFF;

		[[nodiscard]] bool is_null() const noexcept { return index == 0xFFFF; }
	};

	/**
	 * Graph-local name for a buffer used this frame. Minted by create() or import(),
	 * dead after execute(). Resolves to a BufferHandle only inside pass callbacks,
	 * through the PassContext.
	 */
	struct GraphBuffer
	{
		u16 index = 0xFFFF;

		[[nodiscard]] bool is_null() const noexcept { return index == 0xFFFF; }
	};

	/**
	 * A transient target. Pooled by (format, extent, mips, usage); equal descs may
	 * resolve to the same texture in different frames, so contents never survive one.
	 */
	struct GraphTextureDef
	{
		const char* name		  = "graph_texture";
		gpu::TextureFormat format = gpu::TextureFormat::RGBA8Unorm;
		gpu::TextureUsage usage	  = gpu::TextureUsage::Sampled | gpu::TextureUsage::ColorTarget;
		Extent2D extent			  = {};
		u32 mip_count			  = 1;
	};

	/**
	 * A transient buffer. Pooled by (size, usage); equal defs may resolve to the same
	 * buffer in different frames, so contents never survive one. Transients live in
	 * DeviceLocal memory; CPU-visible data goes through the device's transient ring.
	 */
	struct GraphBufferDef
	{
		const char* name	   = "graph_buffer";
		u64 size			   = 0;
		gpu::BufferUsage usage = gpu::BufferUsage::Storage;
	};

	/**
	 * The steady state a texture rests in between passes, mirroring the GPU layer's
	 * resting contract: storage rests writable, sampled rests readable, attachments rest
	 * as targets.
	 */
	[[nodiscard]] constexpr gpu::TextureState resting_state(gpu::TextureUsage usage) noexcept
	{
		using gpu::TextureUsage;

		if ((usage & TextureUsage::Storage) != TextureUsage::None)
			return gpu::TextureState::ShaderWrite;
		if ((usage & TextureUsage::Sampled) != TextureUsage::None)
			return gpu::TextureState::ShaderRead;
		if ((usage & TextureUsage::DepthStencilTarget) != TextureUsage::None)
			return gpu::TextureState::DepthTarget;
		return gpu::TextureState::RenderTarget;
	}

	/**
	 * The buffer counterpart: storage rests writable, indirect args and index
	 * buffers rest at their consumer, everything else rests readable.
	 */
	[[nodiscard]] constexpr gpu::BufferState resting_state(gpu::BufferUsage usage) noexcept
	{
		using gpu::BufferUsage;

		if ((usage & BufferUsage::Storage) != BufferUsage::None)
			return gpu::BufferState::ShaderWrite;
		if ((usage & BufferUsage::Indirect) != BufferUsage::None)
			return gpu::BufferState::IndirectArgument;
		if ((usage & BufferUsage::Index) != BufferUsage::None)
			return gpu::BufferState::IndexBuffer;
		return gpu::BufferState::ShaderRead;
	}

	class RenderGraph;

	/// Hands pass callbacks their resolved resources at record time.
	struct PassContext
	{
		[[nodiscard]] TextureHandle texture(GraphTexture handle) const noexcept;
		[[nodiscard]] BufferHandle buffer(GraphBuffer handle) const noexcept;
		[[nodiscard]] Extent2D extent(GraphTexture handle) const noexcept;
		[[nodiscard]] u64 size(GraphBuffer handle) const noexcept;
		[[nodiscard]] u32 bindless(GraphTexture handle) const noexcept { return bindless_index(texture(handle)); }
		[[nodiscard]] u32 bindless(GraphBuffer handle) const noexcept { return bindless_index(buffer(handle)); }

		const RenderGraph* graph = nullptr;
	};

	class RenderGraph
	{
	public:
		class Pass
		{
		public:
			/// Field-compatible with gpu::ColorAttachment; translation is a copy.
			struct ColorDef
			{
				GraphTexture texture  = {};
				u32 mip				  = 0;
				u32 layer			  = 0;
				gpu::LoadOp load	  = gpu::LoadOp::Clear;
				gpu::StoreOp store	  = gpu::StoreOp::Store;
				gpu::ClearColor clear = {};
			};

			struct DepthDef
			{
				GraphTexture texture = {};
				u32 mip				 = 0;
				u32 layer			 = 0;
				gpu::LoadOp load	 = gpu::LoadOp::Clear;
				gpu::StoreOp store	 = gpu::StoreOp::Store;
				f32 clear_depth		 = 0.0f;
			};

			Pass& read(GraphTexture texture, gpu::TextureState state = gpu::TextureState::ShaderRead) noexcept;
			Pass& read(GraphBuffer buffer, gpu::BufferState state = gpu::BufferState::ShaderRead) noexcept;
			Pass& write(GraphTexture texture, gpu::TextureState state = gpu::TextureState::ShaderWrite) noexcept;
			Pass& write(GraphBuffer buffer, gpu::BufferState state = gpu::BufferState::ShaderWrite) noexcept;
			Pass& color(const ColorDef& def) noexcept;
			Pass& depth(const DepthDef& def) noexcept;

			/**
			 * Runs at execute(), after this pass's barriers, inside the rendering
			 * scope when attachments were declared. Callbacks take (CommandList&)
			 * or (CommandList&, const PassContext&).
			 *
			 * Captures live in the frame arena and are never destroyed: keep them trivial.
			 */
			template <class F> void record(F&& fn) noexcept
			{
				using Fn = std::decay_t<F>;
				static_assert(
					std::is_trivially_destructible_v<Fn>, "captures live in the frame arena and are never destroyed");
				static_assert(
					std::is_invocable_v<Fn&, gpu::CommandList&, const PassContext&> ||
						std::is_invocable_v<Fn&, gpu::CommandList&>,
					"callbacks take (CommandList&) or (CommandList&, const PassContext&)");

				m_record_data =
					new (memory::frame_arena().allocate_fast(sizeof(Fn), alignof(Fn))) Fn(static_cast<F&&>(fn));
				m_record = [](gpu::CommandList& cmd, const PassContext& ctx, void* data)
				{
					Fn& fn = *static_cast<Fn*>(data);
					if constexpr (std::is_invocable_v<Fn&, gpu::CommandList&, const PassContext&>)
						fn(cmd, ctx);
					else
						fn(cmd);
				};
			}

		private:
			friend class RenderGraph;

			struct TextureUse
			{
				GraphTexture texture;
				gpu::TextureState state;
			};

			struct BufferUse
			{
				GraphBuffer buffer;
				gpu::BufferState state;
			};

			using RecordFn = void (*)(gpu::CommandList&, const PassContext&, void*);

			void add_use(GraphTexture texture, gpu::TextureState state) noexcept;
			void add_use(GraphBuffer buffer, gpu::BufferState state) noexcept;

			const char* m_name	 = nullptr;
			RenderGraph* m_graph = nullptr;

			TextureUse m_texture_uses[MAX_PASS_USES] = {};
			u32 m_texture_use_count					 = 0;

			BufferUse m_buffer_uses[MAX_PASS_USES] = {};
			u32 m_buffer_use_count				   = 0;

			ColorDef m_colors[MAX_COLOR_ATTACHMENTS] = {};
			u32 m_color_count						 = 0;

			DepthDef m_depth = {};
			bool m_has_depth = false;

			RecordFn m_record	= nullptr;
			void* m_record_data = nullptr;
		};

		RenderGraph() = default;

		RenderGraph(const RenderGraph&)			   = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;

		/// Destroys pooled textures. Call before the device goes down.
		void shutdown(gpu::Device& device) noexcept;

		/// Opens a frame: forgets last frame's passes, textures and captures.
		void begin() noexcept;

		/// Creates a new GraphTexture
		[[nodiscard]] GraphTexture create(const GraphTextureDef& def) noexcept;

		/// Creates a new GraphBuffer
		[[nodiscard]] GraphBuffer create(const GraphBufferDef& def) noexcept;

		/**
		 * Wraps an externally owned texture (swapchain image, scene asset).
		 *
		 * @param current is its state entering the frame
		 * @param final_state is where the graph leaves it.
		 * @param extent feeds PassContext::extent; imports without one report zero.
		 */
		[[nodiscard]] GraphTexture import(
			TextureHandle texture,
			gpu::TextureState current,
			gpu::TextureState final_state,
			Extent2D extent = {}) noexcept;

		/**
		 * Wraps an externally owned buffer.
		 *
		 * @param current is its state when entering the frame
		 * @param final_state is where the graph leaves it.
		 * @param size feeds PassContext::size; imports without one report zero.
		 */
		[[nodiscard]] GraphBuffer
		import(BufferHandle buffer, gpu::BufferState current, gpu::BufferState final_state, u64 size = 0) noexcept;

		[[nodiscard]] Pass& pass(const char* name) noexcept;

		/**
		 * Resolves transients from the pool, derives barriers, records every pass
		 * into one command list inside a zone named after it, submits, returns every
		 * texture to rest.
		 */
		void execute(gpu::Device& device) noexcept;

	private:
		friend struct PassContext;

		static constexpr u32 UNPOOLED = ~0u;

		struct VirtualTexture
		{
			GraphTextureDef def		  = {};
			TextureHandle physical	  = {};
			gpu::TextureState state	  = gpu::TextureState::Undefined;
			gpu::TextureState resting = gpu::TextureState::Undefined;
			bool imported			  = false;
			u32 pool_slot			  = UNPOOLED;
		};

		struct VirtualBuffer
		{
			GraphBufferDef def		 = {};
			BufferHandle physical	 = {};
			gpu::BufferState state	 = gpu::BufferState::ShaderRead;
			gpu::BufferState resting = gpu::BufferState::ShaderRead;
			bool imported			 = false;
			u32 pool_slot			 = UNPOOLED;
		};

		struct TexturePoolEntry
		{
			TextureHandle texture{};
			u64 key				= 0;
			u32 last_used_frame = 0;
			bool in_use			= false;
		};

		struct BufferPoolEntry
		{
			BufferHandle buffer{};
			u64 size			   = 0;
			gpu::BufferUsage usage = gpu::BufferUsage::None;
			u32 last_used_frame	   = 0;
			bool in_use			   = false;
		};

		[[nodiscard]] TextureHandle acquire(gpu::Device& device, const GraphTextureDef& def, u32& pool_slot) noexcept;
		[[nodiscard]] BufferHandle acquire(gpu::Device& device, const GraphBufferDef& def, u32& pool_slot) noexcept;

		void release(gpu::Device& device) noexcept;

		/// One slot past the cap: declarations past MAX_GRAPH_PASSES land there and never execute.
		Pass m_passes[MAX_GRAPH_PASSES + 1] = {};
		u32 m_pass_count					= 0;

		VirtualTexture m_textures[MAX_GRAPH_TEXTURES] = {};
		u32 m_texture_count							  = 0;

		VirtualBuffer m_buffers[MAX_GRAPH_BUFFERS] = {};
		u32 m_buffer_count						   = 0;

		TexturePoolEntry m_texture_pool[MAX_POOL_ENTRIES] = {};
		BufferPoolEntry m_buffer_pool[MAX_POOL_ENTRIES]	  = {};

		u32 m_frame = 0;
	};
}
