#pragma once

#include <ember/assets/asset.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>

namespace ember
{
	/**
	 * A sampled 2D texture from an image file. Decoded on a worker with stb_image to RGBA8 sRGB
	 * and created streamed: the handle is usable at once and shows the heap's white fallback until
	 * the pixels land, so nothing waits on a load. Device::is_resident() tells when they have.
	 *
	 * The handle is for keeps. A reload decodes the same way and hands the pixels to the device as
	 * an update of this handle, so a material record that took the bindless index at boot draws
	 * the new pixels without anyone touching it. Cooked formats (BC7 with mips) take the same
	 * shape without the decode, later.
	 */
	struct TextureAsset
	{
		TextureHandle texture = {};
		Extent2D extent		  = {};

		static bool load(AssetLoad& load, TextureAsset& out) noexcept;
		static void unload(AssetServices& services, TextureAsset& asset) noexcept;
		static void reload(AssetServices& services, TextureAsset& live, TextureAsset& fresh) noexcept;
	};

	static_assert(AssetType<TextureAsset>);
}
