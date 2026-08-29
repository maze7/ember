#pragma once

#include <ember/containers/span.h>
#include <ember/core/bitmask.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>

#include <glm/vec3.hpp>

namespace ember::gpu
{
	/// Order is ABI: values may be serialized (asset headers, pipeline caches).
	/// Append only; the backend's format table static_asserts the count.
	enum class TextureFormat : u8
	{
		Undefined = 0,
		R8Unorm,
		RG8Unorm,
		RGBA8Unorm,
		RGBA8Srgb,
		BGRA8Unorm,
		BGRA8Srgb,
		R16Float,
		RG16Float,
		RGB32Float, // vertex-pull era; sampling support is NOT guaranteed and is probed at create
		RGBA16Float,
		R32Float,
		RG32Float,
		RGBA32Float,
		R32Uint,
		RG11B10Float,
		RGB10A2Unorm,
		D32Float,
		// Block-compressed.
		BC1Unorm,
		BC1Srgb,
		BC3Unorm,
		BC3Srgb,
		BC4Unorm,
		BC5Unorm,
		BC6HFloat,
		BC7Unorm,
		BC7Srgb,
		Count,
	};

	enum class TextureUsage : u8
	{
		None			   = 0,
		Sampled			   = 1 << 0,
		Storage			   = 1 << 1,
		ColorTarget		   = 1 << 2,
		DepthStencilTarget = 1 << 3,
	};

	EMBER_ENUM_BITWISE_OPS(TextureUsage, u8);

	enum class TextureType : u8
	{
		Texture2D,
		Texture2DArray,
		TextureCube,
		Texture3D,
	};

	/// Defines a single Texture, handed to Device to create.
	struct TextureDef
	{
		const char* name = "texture";
		TextureType type = TextureType::Texture2D;

		// Depth is 1 unless Texture3D; array size lives in `layers`
		Extent3D extent = {1, 1, 1};
		u32 layers		= 1;

		TextureFormat format = TextureFormat::RGBA8Srgb;
		u32 mip_count		 = 1;
		u8 sample_count		 = 1; // > 1 only for attachments (no data, single mip)
		TextureUsage usage	 = TextureUsage::Sampled;

		/// Whole subresource chain, layer-major mip-minor, rows tightly packed
		/// (block rows for BC). Uploaded before this frame's GPU work.
		Span<const u8> initial_data = {};
	};

	/// Returns true if the provided format is a supported Depth format
	[[nodiscard]] constexpr bool is_depth_format(TextureFormat format) noexcept
	{
		return format == TextureFormat::D32Float;
	}

	/// Returns true if the provided format utilizes compression
	[[nodiscard]] constexpr bool is_compressed_format(TextureFormat format) noexcept
	{
		return format >= TextureFormat::BC1Unorm && format <= TextureFormat::BC7Srgb;
	}

	/// Returns the total required mip count for a given Extent3D
	[[nodiscard]] constexpr u32 full_mip_count(Extent3D extent) noexcept
	{
		u32 largest = extent.width > extent.height ? extent.width : extent.height;
		largest		= largest > extent.depth ? largest : extent.depth;

		u32 count = 1;
		while (largest > 1)
		{
			largest >>= 1;
			++count;
		}

		return count;
	}

	// Returns true if the provided TextureDef is valid and usable.
	[[nodiscard]] constexpr bool is_valid(const TextureDef& def) noexcept
	{
		if (def.name == nullptr || def.format == TextureFormat::Undefined || def.format >= TextureFormat::Count)
			return false;

		if (def.extent.width == 0 || def.extent.height == 0 || def.extent.depth == 0 || def.layers == 0)
			return false;

		if (def.usage == TextureUsage::None)
			return false;

		// A texture is a color thing or a depth thing, never both; and depth-stencil
		// memory has no defined CPU-side packing in ember's contract, so no uploads.
		const bool depth = is_depth_format(def.format);
		if (depth != ((def.usage & TextureUsage::DepthStencilTarget) != TextureUsage::None))
			return false;
		if ((def.usage & TextureUsage::ColorTarget) != TextureUsage::None && depth)
			return false;
		if (depth && !def.initial_data.empty())
			return false;

		// Compressed data comes from offline cookers; the GPU never renders into it.
		if (is_compressed_format(def.format) &&
			(def.usage & (TextureUsage::Storage | TextureUsage::ColorTarget)) != TextureUsage::None)
			return false;

		switch (def.type)
		{
			case TextureType::Texture2D:
				if (def.layers != 1 || def.extent.depth != 1)
					return false;
				break;
			case TextureType::Texture2DArray:
				if (def.extent.depth != 1)
					return false;
				break;
			case TextureType::TextureCube:
				if (def.layers != 6 || def.extent.depth != 1)
					return false;
				break;
			case TextureType::Texture3D:
				if (def.layers != 1)
					return false;
				break;
		}

		if (def.mip_count == 0 || def.mip_count > MAX_MIP_LEVELS || def.mip_count > full_mip_count(def.extent))
			return false;

		// MSAA is an attachment-only concept: no mips, no CPU data, by API rule on both targets.
		const bool msaa = def.sample_count > 1;
		if (msaa && (def.sample_count > 8 || (def.sample_count & (def.sample_count - 1)) != 0))
			return false;
		if (msaa && (def.mip_count != 1 || !def.initial_data.empty() || def.type != TextureType::Texture2D))
			return false;

		return true;
	}
}
