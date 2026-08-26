#pragma once

#include <ember/gpu/texture.h>
#include <gpu/vulkan/common.h>
#include <vulkan/vulkan_core.h>

namespace ember::gpu::vk
{
	/**
	 * Everything the backend knows about a TextureFormat, one row per enum value.
	 * Copy math treats every format as blocks: uncompressed formats are 1x1 blocks,
	 * so BC needs no special cases anywhere.
	 */
	struct FormatInfo
	{
		VkFormat vk				  = VK_FORMAT_UNDEFINED;
		u8 block_width			  = 1; // texels per block edge
		u8 block_height			  = 1;
		u8 block_bytes			  = 0;
		VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	};

	// clang-format off
	inline constexpr FormatInfo FORMAT_TABLE[] = {
		/* Undefined    */ {},
		/* R8Unorm      */ {VK_FORMAT_R8_UNORM,                 1, 1,  1},
		/* RG8Unorm     */ {VK_FORMAT_R8G8_UNORM,               1, 1,  2},
		/* RGBA8Unorm   */ {VK_FORMAT_R8G8B8A8_UNORM,           1, 1,  4},
		/* RGBA8Srgb    */ {VK_FORMAT_R8G8B8A8_SRGB,            1, 1,  4},
		/* BGRA8Unorm   */ {VK_FORMAT_B8G8R8A8_UNORM,           1, 1,  4},
		/* BGRA8Srgb    */ {VK_FORMAT_B8G8R8A8_SRGB,            1, 1,  4},
		/* R16Float     */ {VK_FORMAT_R16_SFLOAT,               1, 1,  2},
		/* RG16Float    */ {VK_FORMAT_R16G16_SFLOAT,            1, 1,  4},
		/* RGB32Float   */ {VK_FORMAT_R32G32B32_SFLOAT,         1, 1, 12},
		/* RGBA16Float  */ {VK_FORMAT_R16G16B16A16_SFLOAT,      1, 1,  8},
		/* R32Float     */ {VK_FORMAT_R32_SFLOAT,               1, 1,  4},
		/* RG32Float    */ {VK_FORMAT_R32G32_SFLOAT,            1, 1,  8},
		/* RGBA32Float  */ {VK_FORMAT_R32G32B32A32_SFLOAT,      1, 1, 16},
		/* R32Uint      */ {VK_FORMAT_R32_UINT,                 1, 1,  4},
		/* RG11B10Float */ {VK_FORMAT_B10G11R11_UFLOAT_PACK32,  1, 1,  4},
		/* RGB10A2Unorm */ {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 1,  4},
		/* D32Float     */ {VK_FORMAT_D32_SFLOAT,               1, 1,  4, VK_IMAGE_ASPECT_DEPTH_BIT},
		/* D24UnormS8   */ {VK_FORMAT_D24_UNORM_S8_UINT,        1, 1,  4, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
		/* BC1Unorm     */ {VK_FORMAT_BC1_RGBA_UNORM_BLOCK,     4, 4,  8},
		/* BC1Srgb      */ {VK_FORMAT_BC1_RGBA_SRGB_BLOCK,      4, 4,  8},
		/* BC3Unorm     */ {VK_FORMAT_BC3_UNORM_BLOCK,          4, 4, 16},
		/* BC3Srgb      */ {VK_FORMAT_BC3_SRGB_BLOCK,           4, 4, 16},
		/* BC4Unorm     */ {VK_FORMAT_BC4_UNORM_BLOCK,          4, 4,  8},
		/* BC5Unorm     */ {VK_FORMAT_BC5_UNORM_BLOCK,          4, 4, 16},
		/* BC6HFloat    */ {VK_FORMAT_BC6H_SFLOAT_BLOCK,        4, 4, 16},
		/* BC7Unorm     */ {VK_FORMAT_BC7_UNORM_BLOCK,          4, 4, 16},
		/* BC7Srgb      */ {VK_FORMAT_BC7_SRGB_BLOCK,           4, 4, 16},
	};
	// clang-format on

	static_assert(
		std::size(FORMAT_TABLE) == static_cast<size_t>(TextureFormat::Count),
		"FORMAT_TABLE must mirror TextureFormat one to one");

	// The count assert can't catch a mid-table insertion that shifts rows; these can.
	static_assert(FORMAT_TABLE[static_cast<size_t>(TextureFormat::D32Float)].vk == VK_FORMAT_D32_SFLOAT);
	static_assert(FORMAT_TABLE[static_cast<size_t>(TextureFormat::BC7Srgb)].vk == VK_FORMAT_BC7_SRGB_BLOCK);

	/// Returns the vulkan format info for a given TextureFormat
	[[nodiscard]] constexpr const FormatInfo& format_info(TextureFormat format) noexcept
	{
		return FORMAT_TABLE[static_cast<size_t>(format)];
	}

	/// Tightly packed byte size of one mip of one layer, in whole blocks. This is
	/// the unit the CPU-side data contract and every copy region agree on.
	[[nodiscard]] constexpr u64 subresource_bytes(const FormatInfo& info, Extent3D extent, u32 mip) noexcept
	{
		const u32 width	 = extent.width >> mip > 0 ? extent.width >> mip : 1;
		const u32 height = extent.height >> mip > 0 ? extent.height >> mip : 1;
		const u32 depth	 = extent.depth >> mip > 0 ? extent.depth >> mip : 1;

		const u64 blocks_x = (width + info.block_width - 1) / info.block_width;
		const u64 blocks_y = (height + info.block_height - 1) / info.block_height;

		return blocks_x * blocks_y * depth * info.block_bytes;
	}
}
