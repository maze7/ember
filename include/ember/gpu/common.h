#pragma once

#include <ember/core/common.h>
#include <ember/core/handle.h>

#include <bit>

namespace ember
{
	/**
	 * GPU resource handles.
	 *
	 * Every GPU object is addressed by a 32-bit generational handle (16-bit index + 16-bit generation)
	 * into a pool the Device owns. The tag structs are deliberately incomplete: user code can copy,
	 * compare and store handles, but only the backend can dereference one. Pools are sized once at
	 * device creation and never grow, because a handle's index doubles as the resource's slot in the
	 * bindless descriptor heap.
	 */
	struct Buffer;
	struct Texture;
	struct Sampler;
	struct GraphicsPipeline;
	struct ComputePipeline;
	struct Swapchain;

	using BufferHandle			 = Handle<Buffer, u16>;
	using TextureHandle			 = Handle<Texture, u16>;
	using SamplerHandle			 = Handle<Sampler, u16>;
	using GraphicsPipelineHandle = Handle<GraphicsPipeline, u16>;
	using ComputePipelineHandle	 = Handle<ComputePipeline, u16>;
	using SwapchainHandle		 = Handle<Swapchain, u16>;

	/**
	 * The u32 a shader indexes the bindless heap with. Textures (sampled and storage arrays), samplers
	 * and storage buffers all live at their handle's index. Slot 0 of every array holds a device-owned
	 * fallback, so a null handle (index 0) or a stale index whose slot has been released reads white/zero
	 * instead of faulting.
	 */
	template <class T> [[nodiscard]] constexpr u32 bindless_index(Handle<T, u16> handle) noexcept
	{
		return handle.index;
	}

	/// Contract limits. Shared with shaders/ember.slang. These are part of the binding contract, not
	/// tuning knobs: changing one changes every shader and every backend.
	inline constexpr u32 MAX_FRAMES_IN_FLIGHT  = 3;
	inline constexpr u32 MAX_COLOR_ATTACHMENTS = 8;	 // D3D12 fixed limit; Vulkan clamps via caps.max_color_attachments.
	inline constexpr u32 VERTEX_BUFFER_SLOTS   = 3;	 // position | attributes | instance
	inline constexpr u32 MAX_VERTEX_ATTRIBUTES = 16; //
	inline constexpr u32 CONSTANT_BUFFER_SLOTS = 3;	 // set 1 dynamic UBOs == D3D12 root CBVs
	inline constexpr u32 PUSH_CONSTANT_BYTES   = 32; // 8 root DWORDs; Vulkan guarantees >= 128
	inline constexpr u32 MAX_SWAPCHAINS		   = 8;	 //
	inline constexpr u32 MAX_SWAPCHAIN_IMAGES  = 8;	 //
	inline constexpr u32 MAX_MIP_LEVELS		   = 16; // 65536 * 65536
	inline constexpr u32 MAX_BINDLESS_SAMPLERS = 2048; // D3D12 shader-visible sampler heap ceiling
	inline constexpr u32 ALL_MIPS			   = ~0u;
	inline constexpr u32 ALL_LAYERS			   = ~0u;

#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
	inline constexpr bool GPU_VALIDATION_DEFAULT = true;
#else
	inline constexpr bool GPU_VALIDATION_DEFAULT = false;
#endif

	/// Small API-independent PODs.
	struct Extent2D
	{
		u32 width  = 0;
		u32 height = 0;
	};

	struct Extent3D
	{
		u32 width  = 1;
		u32 height = 1;
		u32 depth  = 1;
	};

	struct Offset3D
	{
		i32 x = 0;
		i32 y = 0;
		i32 z = 0;
	};

	struct Rect2D
	{
		i32 x	   = 0;
		i32 y	   = 0;
		u32 width  = 0;
		u32 height = 0;
	};

	struct Viewport
	{
		f32 x		  = 0.0f;
		f32 y		  = 0.0f;
		f32 width	  = 0.0f;
		f32 height	  = 0.0f;
		f32 min_depth = 0.0f;
		f32 max_depth = 1.0f;
	};
}
