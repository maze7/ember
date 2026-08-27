#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/gpu/sampler.h>
#include <ember/gpu/texture.h>

namespace ember::gpu
{
	enum class PrimitiveTopology : u8
	{
		TriangleList,
		TriangleStrip,
		LineList,
		PointList,
	};

	enum class CullMode : u8
	{
		None,
		Back,
		Front,
	};

	/// Which winding is the front, in the flipped viewport space begin_rendering
	/// establishes. D3D style: Y up in clip space.
	enum class FrontFace : u8
	{
		CounterClockwise,
		Clockwise,
	};

	enum class FillMode : u8
	{
		Solid,
		Wireframe, // requires caps.wireframe; creation fails without it.
	};

	enum class BlendPreset : u8
	{
		Opaque,
		AlphaBlend,			// straight alpha: src.a lerps src over dst
		Additive,			// src.a scales an add; dst alpha preserved
		PremultipliedAlpha, // src already scaled; the compositor preset
	};

	/// One stage of a pipeline. `code` is the backend's blob (SPIR-V); one blob
	/// may serve several stages when it carries multiple entry points.
	struct ShaderStageDef
	{
		Span<const u8> code = {};
		const char* entry	= nullptr;
	};

	/**
	 * Everything a about a graphics pipeline.
	 *
	 * There is deliberately no vertex input state. Vertex data is pulled from
	 * buffer addresses in shaders, so layouts belong to shader code and pipelines
	 * never permute over them.
	 */
	struct GraphicsPipelineDef
	{
		const char* name = "pipeline";

		ShaderStageDef vertex	= {};
		ShaderStageDef fragment = {};

		/// Attachment formats are baked into the pipeline.
		TextureFormat color_formats[MAX_COLOR_ATTACHMENTS] = {};
		u32 color_count									   = 1;
		TextureFormat depth_format						   = TextureFormat::Undefined;

		bool depth_test	 = false;
		bool depth_write = false;

		/// Reverse Z. Depth clears to 0.0, greater is closer.
		CompareOp depth_compare = CompareOp::GreaterEqual;

		CullMode cull			   = CullMode::None;
		FrontFace front			   = FrontFace::CounterClockwise;
		FillMode fill			   = FillMode::Solid;
		BlendPreset blend		   = BlendPreset::Opaque;
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
	};

	[[nodiscard]] constexpr bool is_valid(const ShaderStageDef& stage) noexcept
	{
		// SPIR-V is a stream of 32 bit words.
		return !stage.code.empty() && stage.code.size() % 4 == 0 && stage.entry != nullptr;
	}

	[[nodiscard]] constexpr bool is_valid(const GraphicsPipelineDef& def) noexcept
	{
		if (def.name == nullptr || !is_valid(def.vertex) || !is_valid(def.fragment))
			return false;

		if (def.color_count > MAX_COLOR_ATTACHMENTS)
			return false;

		if (def.color_count == 0 && def.depth_format == TextureFormat::Undefined)
			return false;

		for (u32 i = 0; i < def.color_count; ++i)
		{
			if (def.color_formats[i] == TextureFormat::Undefined || is_depth_format(def.color_formats[i]))
				return false;
		}

		if (def.depth_format != TextureFormat::Undefined && !is_depth_format(def.depth_format))
			return false;

		// Depth state without a depth attachment is a def that lies about itself.
		if ((def.depth_test || def.depth_write) && def.depth_format == TextureFormat::Undefined)
			return false;

		return true;
	}
}
