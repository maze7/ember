#pragma once

#include <ember/core/common.h>
#include <ember/gpu/common.h>

namespace ember::gpu
{
	enum class Filter : u8
	{
		Nearest,
		Linear,
	};

	enum class AddressMode : u8
	{
		Repeat,
		MirroredRepeat,
		ClampToEdge,
		ClampToBorder,
	};

	/// The three colors core Vulkan guarantees. Arbitrary border colors are an
	/// extension on Vulkan and free on D3D12; more can be added when something needs them.
	enum class BorderColor : u8
	{
		TransparentBlack,
		OpaqueBlack,
		OpaqueWhite,
	};

	/// Shared by samplers and depth/stencil state.
	enum class CompareOp : u8
	{
		Never,
		Less,
		Equal,
		LessEqual,
		Greater,
		NotEqual,
		GreaterEqual,
		Always,
	};

	/// Min/Max need caps.sampler_minmax (Hi-Z pyramids); creation falls back to
	/// WeightedAverage with a log when unsupported.
	enum class ReductionMode : u8
	{
		WeightedAverage,
		Min,
		Max
	};

	/// Mirrors VK_LOD_CLAMP_NONE: "no upper clamp" without dragging float limits in.
	inline constexpr f32  LOD_NONE = 1000.0f;

	struct SamplerDef
	{
		const char* name = "sampler";

		Filter min_filter = Filter::Linear;
		Filter mag_filter = Filter::Linear;
		Filter mip_filter = Filter::Linear;

		AddressMode address_u = AddressMode::Repeat;
		AddressMode address_v = AddressMode::Repeat;
		AddressMode address_w = AddressMode::Repeat;

		f32 mip_lod_bias = 0.0f;
		f32 min_lod = 0.0f;
		f32 max_lod = LOD_NONE;

		/// 0/1 = off. Clamped to caps.max_anisotropy at creation.
		u8 max_anisotropy = 0;

		/// PCF shadow samplers. Mutually exclusive with reduction.
		bool compare_enable = false;
		CompareOp compare = CompareOp::Less;

		ReductionMode reduction = ReductionMode::WeightedAverage;
		BorderColor border = BorderColor::OpaqueBlack;
	};

	/// Validates a given SamplerDef and ensures it is usable
	[[nodiscard]] constexpr bool is_valid(const SamplerDef& def) noexcept
	{
		if (def.name == nullptr || def.min_lod > def.max_lod || def.max_anisotropy > 16)
			return false;

		// The vulkan spec forbids min/max reduction combined with depth compare.
		if (def.compare_enable && def.reduction != ReductionMode::WeightedAverage)
			return false;

		return true;
	}
}
