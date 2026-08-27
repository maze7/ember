#include <ember/gpu/device.h>
#include <ember/gpu/texture.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/formats.h>

namespace ember::gpu
{
	namespace
	{
		[[nodiscard]] VkImageUsageFlags to_vk_usage(TextureUsage usage) noexcept
		{
			VkImageUsageFlags flags = 0;

			if ((usage & TextureUsage::Sampled) != TextureUsage::None)
				flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
			if ((usage & TextureUsage::Storage) != TextureUsage::None)
				flags |= VK_IMAGE_USAGE_STORAGE_BIT;
			if ((usage & TextureUsage::ColorTarget) != TextureUsage::None)
				flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if ((usage & TextureUsage::DepthStencilTarget) != TextureUsage::None)
				flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

			return flags;
		}

		/// What the adapter must support in optimal tiling for this usage to work at all.
		[[nodiscard]] VkFormatFeatureFlags required_features(TextureUsage usage) noexcept
		{
			VkFormatFeatureFlags features = 0;

			if ((usage & TextureUsage::Sampled) != TextureUsage::None)
				features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			if ((usage & TextureUsage::Storage) != TextureUsage::None)
				features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			if ((usage & TextureUsage::ColorTarget) != TextureUsage::None)
				features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
			if ((usage & TextureUsage::DepthStencilTarget) != TextureUsage::None)
				features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

			return features;
		}

		[[nodiscard]] VkImageType to_vk_type(TextureType type) noexcept
		{
			return type == TextureType::Texture3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
		}

		[[nodiscard]] VkImageViewType to_vk_view_type(TextureType type) noexcept
		{
			switch (type)
			{
				case TextureType::Texture2D:
					return VK_IMAGE_VIEW_TYPE_2D;
				case TextureType::Texture2DArray:
					return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
				case TextureType::TextureCube:
					return VK_IMAGE_VIEW_TYPE_CUBE;
				case TextureType::Texture3D:
					return VK_IMAGE_VIEW_TYPE_3D;
			}
			return VK_IMAGE_VIEW_TYPE_2D;
		}
	}

	TextureHandle Device::create_texture(const TextureDef& def) noexcept
	{
		EMBER_GPU_GUARD({});

		if (!::ember::gpu::is_valid(def))
		{
			EMBER_ERROR("gpu: texture '{}' has an invalid def", def.name);
			return {};
		}

		if (!def.initial_data.empty())
		{
			EMBER_ERROR(
				"gpu: texture '{}' initial_data arrives with update_texture; create it empty for now", def.name);
			return {};
		}

		const vk::FormatInfo& info = vk::format_info(def.format);

		// Optimal tiling support is per adapter, per format, per usage; the spec only
		// guarantees a baseline. RGB32Float sampling is the canonical hole.
		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(m_backend->context.adapter, info.vk, &props);

		const VkFormatFeatureFlags needed = required_features(def.usage);
		if ((props.optimalTilingFeatures & needed) != needed)
		{
			EMBER_ERROR("gpu: texture '{}' format unsupported for the requested usage on this adapter", def.name);
			return {};
		}

		const VkImageCreateInfo image_info{
			.sType		   = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.flags		   = def.type == TextureType::TextureCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
			.imageType	   = to_vk_type(def.type),
			.format		   = info.vk,
			.extent		   = {def.extent.width, def.extent.height, def.extent.depth},
			.mipLevels	   = def.mip_count,
			.arrayLayers   = def.layers,
			.samples	   = static_cast<VkSampleCountFlagBits>(def.sample_count),
			.tiling		   = VK_IMAGE_TILING_OPTIMAL,
			.usage		   = to_vk_usage(def.usage),
			.sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		VmaAllocationCreateInfo alloc_info{};
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		VkImage image			 = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;

		if (auto vr =
				vmaCreateImage(m_backend->context.allocator, &image_info, &alloc_info, &image, &allocation, nullptr);
			vr != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: texture '{}' creation failed: {}", def.name, vk::result_name(vr));
			return {};
		}

		const VkImageViewCreateInfo view_info{
			.sType			  = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image			  = image,
			.viewType		  = to_vk_view_type(def.type),
			.format			  = info.vk,
			.subresourceRange = {info.aspect, 0, def.mip_count, 0, def.layers},
		};

		VkImageView view = VK_NULL_HANDLE;
		if (auto vr = vkCreateImageView(m_backend->context.device, &view_info, nullptr, &view); vr != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: texture '{}' view creation failed: {}", def.name, vk::result_name(vr));
			vmaDestroyImage(m_backend->context.allocator, image, allocation);
			return {};
		}

		// One view serves sampling and attachment duty. The storage view (and the sRGB
		// reinterpretation it needs) joins with the bindless writes.
		const TextureHandle handle = m_backend->resources.textures.insert(
			vk::TextureHot{.image = image, .sampled_view = view},
			vk::TextureCold{
				.allocation	 = allocation,
				.extent		 = image_info.extent,
				.format		 = info.vk,
				.api_format	 = def.format,
				.mip_count	 = def.mip_count,
				.layer_count = def.layers,
			});

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: texture pool exhausted ({})", def.name);
			vkDestroyImageView(m_backend->context.device, view, nullptr);
			vmaDestroyImage(m_backend->context.allocator, image, allocation);
			return {};
		}

		vk::set_name(m_backend->context, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(image), def.name);

		return handle;
	}

	void Device::destroy(TextureHandle handle) noexcept
	{
		EMBER_GPU_GUARD();

		vk::TextureHot* hot = m_backend->resources.textures.try_get(handle);
		if (hot == nullptr)
			return;

		const vk::TextureCold& cold = m_backend->resources.textures.get_cold(handle);
		if (!cold.owns_image)
		{
			EMBER_ASSERT(false && "backbuffers are destroyed through their swapchain");
			return;
		}

		m_backend->destroy_queue.destroy(hot->sampled_view);
		m_backend->destroy_queue.destroy(hot->storage_view);
		m_backend->destroy_queue.destroy(hot->image, cold.allocation);
		(void)m_backend->resources.textures.erase(handle);
	}

	bool Device::is_valid(TextureHandle handle) const noexcept
	{
		return m_backend != nullptr && m_backend->resources.textures.contains(handle);
	}
}
