#include <ember/assets/texture_asset.h>
#include <ember/core/logger.h>
#include <ember/gpu/device.h>

#include <stb_image.h>

namespace ember
{
	bool TextureAsset::load(AssetLoad& load, TextureAsset& out) noexcept
	{
		const Span<const u8> file = load.bytes();

		int width	 = 0;
		int height	 = 0;
		int channels = 0;

		stbi_uc* pixels =
			stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels, 4);

		if (pixels == nullptr)
		{
			EMBER_ERROR("texture '{}': {}", load.path(), stbi_failure_reason());
			return false;
		}

		const gpu::TextureDef def{
			.name		  = load.path().data(),
			.extent		  = {static_cast<u32>(width), static_cast<u32>(height), 1},
			.format		  = gpu::TextureFormat::RGBA8Srgb,
			.mip_count	  = 1,
			.usage		  = gpu::TextureUsage::Sampled,
			.initial_data = {pixels, size_t{static_cast<u32>(width)} * static_cast<u32>(height) * 4},
			.streamed	  = true,
		};

		out.extent = {def.extent.width, def.extent.height};

		bool ok = false;

		// A reload lands behind the handle everyone holds; the fresh payload carries only the
		// shape, so the unload it gets later has nothing to destroy. A first load makes the
		// handle, streamed: usable at once, the fallback until the pixels land.
		if (const TextureAsset* live = load.live<TextureAsset>())
		{
			ok = load.gpu().update_texture(live->texture, def);
		}
		else
		{
			out.texture = load.gpu().create_texture(def);
			ok			= !out.texture.is_null();
		}

		stbi_image_free(pixels);
		return ok;
	}

	void TextureAsset::unload(AssetServices& services, TextureAsset& asset) noexcept
	{
		services.gpu.destroy(asset.texture);
	}

	void TextureAsset::reload(AssetServices&, TextureAsset& live, TextureAsset& fresh) noexcept
	{
		// The image already moved on the device; only the shape is left to carry over.
		live.extent = fresh.extent;
	}
}
