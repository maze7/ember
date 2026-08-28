#include <ember/gpu/device.h>
#include <ember/gpu/sampler.h>
#include <gpu/vulkan/backend.h>

namespace ember::gpu
{
	namespace
	{
		[[nodiscard]] VkFilter to_vk(Filter filter) noexcept
		{
			return filter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		}

		[[nodiscard]] VkSamplerAddressMode to_vk(AddressMode mode) noexcept
		{
			switch (mode)
			{
				case AddressMode::Repeat:
					return VK_SAMPLER_ADDRESS_MODE_REPEAT;
				case AddressMode::MirroredRepeat:
					return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
				case AddressMode::ClampToEdge:
					return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				case AddressMode::ClampToBorder:
					return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			}
			return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		}

		[[nodiscard]] VkBorderColor to_vk(BorderColor color) noexcept
		{
			switch (color)
			{
				case BorderColor::TransparentBlack:
					return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
				case BorderColor::OpaqueBlack:
					return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
				case BorderColor::OpaqueWhite:
					return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
			}
			return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
		}
	}

	SamplerHandle Device::create_sampler(const SamplerDef& def) noexcept
	{
		EMBER_GPU_GUARD({});

		if (!::ember::gpu::is_valid(def))
		{
			EMBER_ERROR("gpu: sampler '{}' has an invalid def", def.name);
			return {};
		}

		const Context& ctx = m_backend->context;

		VkSamplerReductionModeCreateInfo reduction_info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
			.reductionMode =
				def.reduction == ReductionMode::Min ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX,
		};

		const void* next = nullptr;
		if (def.reduction != ReductionMode::WeightedAverage)
		{
			if (ctx.caps.sampler_minmax)
				next = &reduction_info;
			else
				EMBER_WARN("gpu: sampler '{}': min/max reduction unsupported, using average", def.name);
		}

		const u32 anisotropy =
			def.max_anisotropy > ctx.caps.max_anisotropy ? ctx.caps.max_anisotropy : def.max_anisotropy;

		const VkSamplerCreateInfo info{
			.sType	   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.pNext	   = next,
			.magFilter = to_vk(def.mag_filter),
			.minFilter = to_vk(def.min_filter),
			.mipmapMode =
				def.mip_filter == Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU	  = to_vk(def.address_u),
			.addressModeV	  = to_vk(def.address_v),
			.addressModeW	  = to_vk(def.address_w),
			.mipLodBias		  = def.mip_lod_bias,
			.anisotropyEnable = anisotropy > 1 ? VK_TRUE : VK_FALSE,
			.maxAnisotropy	  = static_cast<f32>(anisotropy > 1 ? anisotropy : 1),
			.compareEnable	  = def.compare_enable ? VK_TRUE : VK_FALSE,
			.compareOp		  = vk::to_vk_compare(def.compare),
			.minLod			  = def.min_lod,
			.maxLod			  = def.max_lod,
			.borderColor	  = to_vk(def.border),
		};

		VkSampler sampler = VK_NULL_HANDLE;
		if (auto vr = vkCreateSampler(ctx.device, &info, nullptr, &sampler); vr != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: sampler '{}' creation failed: {}", def.name, vk::result_name(vr));
			return {};
		}

		const SamplerHandle handle = m_backend->resources.samplers.insert(vk::SamplerData{.handle = sampler});

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: sampler pool exhausted ({})", def.name);
			vkDestroySampler(ctx.device, sampler, nullptr);
			return {};
		}

		m_backend->descriptor_heap.write_sampler(ctx, handle.index, sampler);
		vk::set_name(ctx, VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<u64>(sampler), def.name);

		return handle;
	}

	void Device::destroy(SamplerHandle handle) noexcept
	{
		EMBER_GPU_GUARD();

		vk::SamplerData* data = m_backend->resources.samplers.try_get(handle);
		if (data == nullptr)
			return;

		m_backend->destroy_queue.destroy(data->handle);
		m_backend->destroy_queue.reset_slot(handle.index, vk::HeapArray::Sampler);
		(void)m_backend->resources.samplers.erase(handle);
	}

	bool Device::is_valid(SamplerHandle handle) const noexcept
	{
		return m_backend != nullptr && m_backend->resources.samplers.contains(handle);
	}
}
