#include <ember/gpu/device.h>
#include <ember/gpu/texture.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/formats.h>

#include <algorithm>

namespace ember::gpu
{
	namespace
	{
		[[nodiscard]] VkImageUsageFlags to_vk_usage(TextureUsage usage) noexcept
		{
			// TRANSFER_DST always: initial_data, update_texture and the christening
			// barrier all need it, and unlike STORAGE it costs no compression.
			VkImageUsageFlags flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

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

		/**
		 * Where a texture rests between uses. Creation transitions into it, the
		 * descriptor records it, and later passes must return to it. Storage wins
		 * over Sampled because storage access is only legal in GENERAL.
		 */
		[[nodiscard]] VkImageLayout steady_layout(TextureUsage usage) noexcept
		{
			if ((usage & TextureUsage::Storage) != TextureUsage::None)
				return VK_IMAGE_LAYOUT_GENERAL;
			if ((usage & TextureUsage::Sampled) != TextureUsage::None)
				return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			if ((usage & TextureUsage::DepthStencilTarget) != TextureUsage::None)
				return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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

		const vk::FormatInfo& info = vk::format_info(def.format);

		// Content validation the def validator can't do. A corrupted asset is an
		// error and a reject, never an assert in release.
		if (!def.initial_data.empty())
		{
			u64 expected = 0;
			for (u32 mip = 0; mip < def.mip_count; ++mip)
				expected += vk::subresource_bytes(info, def.extent, mip);
			expected *= def.layers;

			if (def.initial_data.size() != expected)
			{
				EMBER_ERROR(
					"gpu: texture '{}' initial_data is {} bytes, the subresource chain needs {}",
					def.name,
					def.initial_data.size(),
					expected);
				return {};
			}
		}

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
			.sType	   = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.flags	   = def.type == TextureType::TextureCube ? VkImageCreateFlags{VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT}
															  : VkImageCreateFlags{},
			.imageType = to_vk_type(def.type),
			.format	   = info.vk,
			.extent	   = {def.extent.width, def.extent.height, def.extent.depth},
			.mipLevels = def.mip_count,
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

		// Storage access addresses one mip; mip 0 is the contract until per-mip
		// views arrive. Cube storage views must be 2D arrays.
		VkImageView storage_view = VK_NULL_HANDLE;
		if ((def.usage & TextureUsage::Storage) != TextureUsage::None)
		{
			VkImageViewCreateInfo storage_info = view_info;
			storage_info.viewType =
				def.type == TextureType::TextureCube ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : view_info.viewType;
			storage_info.subresourceRange.levelCount = 1;

			if (vkCreateImageView(m_backend->context.device, &storage_info, nullptr, &storage_view) != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: texture '{}' storage view creation failed", def.name);
				vkDestroyImageView(m_backend->context.device, view, nullptr);
				vmaDestroyImage(m_backend->context.allocator, image, allocation);
				return {};
			}
		}

		const VkImageLayout steady = steady_layout(def.usage);

		const TextureHandle handle = m_backend->resources.textures.insert(
			vk::TextureHot{
				.image		  = image,
				.sampled_view = view,
				.storage_view = storage_view,
			},
			vk::TextureCold{
				.allocation	 = allocation,
				.extent		 = image_info.extent,
				.format		 = info.vk,
				.api_format	 = def.format,
				.mip_count	 = def.mip_count,
				.layer_count = def.layers,
				.layout		 = steady,
				.type		 = def.type,
			});

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: texture pool exhausted ({})", def.name);
			vkDestroyImageView(m_backend->context.device, view, nullptr);
			vmaDestroyImage(m_backend->context.allocator, image, allocation);
			return {};
		}

		// A fresh slot is unreferenced by any in-flight frame, so writing now is safe.
		if ((def.usage & TextureUsage::Sampled) != TextureUsage::None)
			m_backend->descriptor_heap.write_sampled(m_backend->context, handle.index, view, steady, def.type);
		if ((def.usage & TextureUsage::Storage) != TextureUsage::None)
			m_backend->descriptor_heap.write_storage(m_backend->context, handle.index, storage_view, def.type);

		vk::TextureCold& cold = *m_backend->resources.textures.get_cold(handle);

		// Deep mips get their own storage entries so shaders can write any level by
		// index. Hidden pool slots: created with the texture, destroyed with it,
		// never handed out as user handles.
		if ((def.usage & TextureUsage::Storage) != TextureUsage::None)
		{
			for (u32 mip = 1; mip < def.mip_count; ++mip)
			{
				VkImageViewCreateInfo mip_info = view_info;
				mip_info.viewType =
					def.type == TextureType::TextureCube ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : view_info.viewType;
				mip_info.subresourceRange.baseMipLevel = mip;
				mip_info.subresourceRange.levelCount   = 1;

				VkImageView mip_view = VK_NULL_HANDLE;
				if (vkCreateImageView(m_backend->context.device, &mip_info, nullptr, &mip_view) != VK_SUCCESS)
				{
					EMBER_ERROR("gpu: texture '{}' mip {} storage view creation failed", def.name, mip);
					destroy(handle);
					return {};
				}

				const TextureHandle entry = m_backend->resources.textures.insert(
					vk::TextureHot{.image = image, .storage_view = mip_view},
					vk::TextureCold{
						.extent =
							{
								std::max(def.extent.width >> mip, 1u),
								std::max(def.extent.height >> mip, 1u),
								std::max(def.extent.depth >> mip, 1u),
							},
						.format		= info.vk,
						.api_format = def.format,
						.layout		= steady,
						.owns_image = false,
						.type		= def.type,
						.parent		= handle,
					});

				if (entry.is_null())
				{
					EMBER_ERROR("gpu: texture pool exhausted on '{}' mip {}", def.name, mip);
					vkDestroyImageView(m_backend->context.device, mip_view, nullptr);
					destroy(handle);
					return {};
				}

				cold.mip_storage[mip] = entry;
				m_backend->descriptor_heap.write_storage(m_backend->context, entry.index, mip_view, def.type);
			}
		}

		// Render targets with more than one subresource carry single slice views for
		// begin_rendering to pick by (mip, layer). Bounded so a config mistake fails
		// at creation with a message.
		const bool target =
			(def.usage & (TextureUsage::ColorTarget | TextureUsage::DepthStencilTarget)) != TextureUsage::None;

		if (target && (def.mip_count > 1 || def.layers > 1))
		{
			if (def.mip_count * def.layers > 128)
			{
				EMBER_ERROR(
					"gpu: texture '{}' attachment matrix {}x{} exceeds 128 slices", def.name, def.mip_count, def.layers);
				destroy(handle);
				return {};
			}

			cold.attachment_views.reserve(def.mip_count * def.layers);

			for (u32 mip = 0; mip < def.mip_count; ++mip)
			{
				for (u32 layer = 0; layer < def.layers; ++layer)
				{
					VkImageViewCreateInfo slice_info		   = view_info;
					slice_info.viewType						   = VK_IMAGE_VIEW_TYPE_2D;
					slice_info.subresourceRange.baseMipLevel   = mip;
					slice_info.subresourceRange.levelCount	   = 1;
					slice_info.subresourceRange.baseArrayLayer = layer;
					slice_info.subresourceRange.layerCount	   = 1;

					VkImageView slice = VK_NULL_HANDLE;
					if (vkCreateImageView(m_backend->context.device, &slice_info, nullptr, &slice) != VK_SUCCESS)
					{
						EMBER_ERROR("gpu: texture '{}' attachment view creation failed", def.name);
						destroy(handle);
						return {};
					}

					cold.attachment_views.push_back(slice);
				}
			}
		}

		// Data lands and/or the image transitions into its steady layout; either way
		// every texture leaves creation resting in a known layout.
		vk::staging_upload_texture(
			*m_backend,
			{
				.image		 = image,
				.format		 = def.format,
				.extent		 = def.extent,
				.mip_count	 = def.mip_count,
				.layer_count = def.layers,
				.steady		 = steady,
			},
			def.initial_data);

		vk::set_name(m_backend->context, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(image), def.name);

		return handle;
	}

	void Device::destroy(TextureHandle handle) noexcept
	{
		EMBER_GPU_GUARD();

		vk::TextureHot* hot = m_backend->resources.textures.get(handle);
		if (hot == nullptr)
			return;

		const vk::TextureCold& cold = *m_backend->resources.textures.get_cold(handle);

		EMBER_ASSERT(cold.parent.is_null() && "internal subresource entries die with their texture");

		if (!cold.owns_image)
		{
			EMBER_ASSERT(false && "backbuffers are destroyed through their swapchain");
			return;
		}

		// Internal subresources ride the same clock as the texture itself.
		for (const TextureHandle entry : cold.mip_storage)
		{
			if (entry.is_null())
				continue;

			m_backend->destroy_queue.destroy(m_backend->resources.textures.get(entry)->storage_view);
			m_backend->destroy_queue.reset_slot(entry.index, vk::HeapArray::Storage);
			(void)m_backend->resources.textures.retire(entry);
		}

		for (const VkImageView slice : cold.attachment_views)
			m_backend->destroy_queue.destroy(slice);

		m_backend->destroy_queue.destroy(hot->sampled_view);
		m_backend->destroy_queue.destroy(hot->storage_view);
		m_backend->destroy_queue.destroy(hot->image, cold.allocation);
		m_backend->destroy_queue.reset_slot(handle.index, vk::HeapArray::Sampled | vk::HeapArray::Storage);

		// Retire only: the drain that resets this slot's descriptor releases it, so
		// the index cannot be reclaimed while an in-flight frame can still read it.
		(void)m_backend->resources.textures.retire(handle);
	}

	bool Device::is_valid(TextureHandle handle) const noexcept
	{
		return m_backend != nullptr && m_backend->resources.textures.contains(handle);
	}

	void Device::update_texture(TextureHandle handle, u32 mip, u32 layer, Span<const u8> data) noexcept
	{
		EMBER_GPU_GUARD();

		if (data.empty())
			return;

		const vk::TextureCold* cold = m_backend->resources.textures.get_cold(handle);

		if (cold == nullptr || !cold->owns_image)
		{
			EMBER_ERROR("gpu: update_texture on a stale or backbuffer handle");
			return;
		}

		if (mip >= cold->mip_count || layer >= cold->layer_count)
		{
			EMBER_ERROR("gpu: update_texture subresource out of range (mip {}, layer {})", mip, layer);
			return;
		}

		const Extent3D extent{cold->extent.width, cold->extent.height, cold->extent.depth};
		const u64 expected = vk::subresource_bytes(vk::format_info(cold->api_format), extent, mip);

		if (data.size() != expected)
		{
			EMBER_ERROR("gpu: update_texture data is {} bytes, mip {} needs {}", data.size(), mip, expected);
			return;
		}

		vk::staging_update_texture(
			*m_backend,
			{
				.image		 = m_backend->resources.textures.get(handle)->image,
				.format		 = cold->api_format,
				.extent		 = extent,
				.mip_count	 = cold->mip_count,
				.layer_count = cold->layer_count,
				.steady		 = cold->layout,
			},
			mip,
			layer,
			data);
	}

	u32 Device::storage_index(TextureHandle handle, u32 mip) const noexcept
	{
		if (m_backend == nullptr)
			return 0;

		const vk::TextureCold* cold = m_backend->resources.textures.get_cold(handle);
		if (cold == nullptr)
			return 0; // slot 0 is the fallback; a stale handle degrades instead of faulting

		EMBER_ASSERT(mip < cold->mip_count);

		if (mip == 0)
			return handle.index;

		EMBER_ASSERT(!cold->mip_storage[mip].is_null() && "texture was not created with Storage usage");
		return cold->mip_storage[mip].index;
	}
}
