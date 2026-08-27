#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>

#include <type_traits>

namespace ember::gpu
{
	struct Backend;

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
		CopySrc,	  //
		CopyDst,	  //
		Present,	  // only legal as `after`; the presentation engine takes over from here
	};

	struct TextureBarrier
	{
		TextureHandle texture{};
		TextureState before = TextureState::Undefined;
		TextureState after	= TextureState::Undefined;
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
		LoadOp load	  = LoadOp::Clear;
		StoreOp store = StoreOp::Store;
		ClearColor clear{};
	};

	struct DepthAttachment
	{
		TextureHandle texture{}; // null = no depth
		LoadOp load		= LoadOp::Clear;
		StoreOp store	= StoreOp::Store;
		f32 clear_depth = 0.0f; // reverse Z: zero is the far plane
	};

	struct RenderingDef
	{
		Span<const ColorAttachment> colors = {};
		DepthAttachment depth{};
	};

	/**
	 * Records GPU work inside a frame. Device::begin_command_list() hands one out,
	 * Device::submit() seals it; the render area, viewport and scissor come from the attachments,
	 * flipped so clip space is Y up on every backend.
	 *
	 * A CommandList is a view over device state, cheap to copy; a submitted or default
	 * constructed list ignores every call.
	 */
	class CommandList
	{
	public:
		void barrier(Span<const TextureBarrier> barriers) noexcept;
		void barrier(const TextureBarrier& single) noexcept { barrier({&single, 1}); }

		void begin_rendering(const RenderingDef& def) noexcept;
		void end_rendering() noexcept;

		void set_pipeline(GraphicsPipelineHandle pipeline) noexcept;

		template <typename T> void set_push_constants(const T& data) noexcept
		{
			static_assert(std::is_trivially_copyable_v<T>, "push constants are raw bytes");
			static_assert(sizeof(T) <= PUSH_CONSTANT_BYTES, "push block exceeds the contract");
			push(&data, sizeof(T));
		}

		void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0, u32 first_instance = 0) noexcept;

	private:
		friend class Device;

		void push(const void* data, u32 size) noexcept;

		Backend* m_backend = nullptr;
	};
}
