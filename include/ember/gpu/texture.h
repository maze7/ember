#pragma once

#include <ember/containers/span.h>
#include <ember/core/bitmask.h>
#include <ember/core/common.h>

#include <glm/vec3.hpp>

namespace ember
{
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
		RGB32Float,
		RGBA16Float,
		R32Float,
		RG32Float,
		RGBA32Float,
		R32Uint,
		RG11B10Float,
		RGB10A2Unorm,
		D32Float,
		D24UnormS8,
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

	struct TextureDef
	{
		const char* name	  = "texture";
		TextureType type	  = TextureType::Texture2D;
		glm::uvec3 dimensions = {1, 1, 1}; // Width, Height, Depth or Layers
		TextureFormat format  = TextureFormat::RGBA8Srgb;
		u32 mip_count		  = 1;
		u8 sample_count		  = 1;
		TextureUsage usage	  = TextureUsage::Sampled;

		// Optional initial data
		Span<const u8> initial_data = {};
	};
}
