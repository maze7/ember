#include "ember/gpu/common.h"
#include <ember/gpu/buffer.h>
#include <ember/gpu/sampler.h>
#include <ember/gpu/texture.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/descriptor_heap.h>
#include <vulkan/vulkan_core.h>

namespace ember::gpu::vk
{
	namespace
	{
		constexpr VkDescriptorBindingFlags BINDLESS_FLAGS = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
															VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
															VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

		void write_image(
			const Context& ctx,
			VkDescriptorSet set,
			u32 binding,
			u32 slot,
			VkDescriptorType type,
			VkImageView view,
			VkSampler sampler,
			VkImageLayout layout) noexcept
		{
			const VkDescriptorImageInfo info{sampler, view, layout};

			const VkWriteDescriptorSet write{
				.sType			 = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet			 = set,
				.dstBinding		 = binding,
				.dstArrayElement = slot,
				.descriptorCount = 1,
				.descriptorType	 = type,
				.pImageInfo		 = &info,
			};

			vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
		}

	}

	bool
	DescriptorHeap::init(const Context& ctx, u32 texture_capacity, u32 sampler_capacity, u32 buffer_capacity) noexcept
	{
		m_texture_capacity = texture_capacity;
		m_sampler_capacity = sampler_capacity;
		m_buffer_capacity  = buffer_capacity;

		// Set O: the four bindless arrays, sized to the pools so a handle's index is its slot.
		// Update-after-bind lets creation write descriptors while earlier frames still render.
		const VkDescriptorSetLayoutBinding heap_bindings[] = {
			{BINDING_SAMPLED_IMAGES, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, texture_capacity, VK_SHADER_STAGE_ALL, nullptr},
			{BINDING_STORAGE_IMAGES, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, texture_capacity, VK_SHADER_STAGE_ALL, nullptr},
			{BINDING_SAMPLERS, VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity, VK_SHADER_STAGE_ALL, nullptr},
			{BINDING_STORAGE_BUFFERS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_capacity, VK_SHADER_STAGE_ALL, nullptr},
		};

		const VkDescriptorBindingFlags binding_flags[] = {
			BINDLESS_FLAGS, BINDLESS_FLAGS, BINDLESS_FLAGS, BINDLESS_FLAGS};

		const VkDescriptorSetLayoutBindingFlagsCreateInfo heap_flags{
			.sType		   = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			.bindingCount  = static_cast<u32>(std::size(heap_bindings)),
			.pBindingFlags = binding_flags,
		};

		const VkDescriptorSetLayoutCreateInfo heap_info{
			.sType		  = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext		  = &heap_flags,
			.flags		  = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			.bindingCount = static_cast<u32>(std::size(heap_bindings)),
			.pBindings	  = heap_bindings,
		};

		if (vkCreateDescriptorSetLayout(ctx.device, &heap_info, nullptr, &m_heap_layout) != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: bindless heap layout creation failed");
			return false;
		}

		// Set 1: the dynamic constant slots. Descriptors are written once against the
		// transient ring; draws vary only the dynamic offsets.
		VkDescriptorSetLayoutBinding constant_bindings[CONSTANT_BUFFER_SLOTS];
		for (u32 i = 0; i < CONSTANT_BUFFER_SLOTS; ++i)
			constant_bindings[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, nullptr};

		const VkDescriptorSetLayoutCreateInfo constants_info{
			.sType		  = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = CONSTANT_BUFFER_SLOTS,
			.pBindings	  = constant_bindings,
		};

		if (vkCreateDescriptorSetLayout(ctx.device, &constants_info, nullptr, &m_constants_layout) != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: constants layout creation failed");
			return false;
		}

		// The one pipeline layout in the engine. Every pipeline compiles against it, so
		// switching pipelines never disturbs bindings, and push constants formt he whole
		// per draw ABI.
		const VkDescriptorSetLayout set_layouts[] = {m_heap_layout, m_constants_layout};

		const VkPushConstantRange push_range{
			.stageFlags = VK_SHADER_STAGE_ALL,
			.offset		= 0,
			.size		= PUSH_CONSTANT_BYTES,
		};

		const VkPipelineLayoutCreateInfo layout_info{
			.sType					= VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount			= static_cast<u32>(std::size(set_layouts)),
			.pSetLayouts			= set_layouts,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges	= &push_range,
		};

		if (vkCreatePipelineLayout(ctx.device, &layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: pipeline layout creation failed");
			return false;
		}

		const VkDescriptorPoolSize pool_sizes[] = {
			{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, texture_capacity},
			{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, texture_capacity},
			{VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_capacity},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, CONSTANT_BUFFER_SLOTS},
		};

		const VkDescriptorPoolCreateInfo pool_info{
			.sType		   = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags		   = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
			.maxSets	   = 2,
			.poolSizeCount = static_cast<u32>(std::size(pool_sizes)),
			.pPoolSizes	   = pool_sizes,
		};

		if (vkCreateDescriptorPool(ctx.device, &pool_info, nullptr, &m_pool) != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: descriptor pool creation failed");
			return false;
		}

		const VkDescriptorSetAllocateInfo alloc_info{
			.sType				= VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool		= m_pool,
			.descriptorSetCount = static_cast<u32>(std::size(set_layouts)),
			.pSetLayouts		= set_layouts,
		};

		VkDescriptorSet sets[2] = {};
		if (vkAllocateDescriptorSets(ctx.device, &alloc_info, sets) != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: descriptor set allocation failed");
			return false;
		}

		m_set		= sets[0];
		m_constants = sets[1];

		set_name(ctx, VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<u64>(m_set), "ember.bindless_heap");
		set_name(ctx, VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<u64>(m_constants), "ember.constants");

		return true;
	}

	void DescriptorHeap::destroy(const Context& ctx) noexcept
	{
		// The pool owns both sets. Every vkDestroy tolerates null, so partial boots
		// tear down through here unconditionally.
		vkDestroyDescriptorPool(ctx.device, m_pool, nullptr);
		vkDestroyPipelineLayout(ctx.device, m_pipeline_layout, nullptr);
		vkDestroyDescriptorSetLayout(ctx.device, m_heap_layout, nullptr);
		vkDestroyDescriptorSetLayout(ctx.device, m_constants_layout, nullptr);

		m_pool			   = VK_NULL_HANDLE;
		m_pipeline_layout  = VK_NULL_HANDLE;
		m_heap_layout	   = VK_NULL_HANDLE;
		m_constants_layout = VK_NULL_HANDLE;
		m_set			   = VK_NULL_HANDLE;
		m_constants		   = VK_NULL_HANDLE;
	}

	void DescriptorHeap::write_sampled(const Context& ctx, u32 slot, VkImageView view, VkImageLayout layout) noexcept
	{
		EMBER_ASSERT(slot < m_texture_capacity);
		write_image(
			ctx, m_set, BINDING_SAMPLED_IMAGES, slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, VK_NULL_HANDLE, layout);
	}

	void DescriptorHeap::write_storage(const Context& ctx, u32 slot, VkImageView view) noexcept
	{
		EMBER_ASSERT(slot < m_texture_capacity);
		write_image(
			ctx,
			m_set,
			BINDING_STORAGE_IMAGES,
			slot,
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			view,
			VK_NULL_HANDLE,
			VK_IMAGE_LAYOUT_GENERAL);
	}

	void DescriptorHeap::write_sampler(const Context& ctx, u32 slot, VkSampler sampler) noexcept
	{
		EMBER_ASSERT(slot < m_sampler_capacity);
		write_image(
			ctx,
			m_set,
			BINDING_SAMPLERS,
			slot,
			VK_DESCRIPTOR_TYPE_SAMPLER,
			VK_NULL_HANDLE,
			sampler,
			VK_IMAGE_LAYOUT_UNDEFINED);
	}

	void DescriptorHeap::write_buffer(const Context& ctx, u32 slot, VkBuffer buffer, u64 size) noexcept
	{
		EMBER_ASSERT(slot < m_buffer_capacity);

		const VkDescriptorBufferInfo info{buffer, 0, size};

		const VkWriteDescriptorSet write{
			.sType			 = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet			 = m_set,
			.dstBinding		 = BINDING_STORAGE_BUFFERS,
			.dstArrayElement = slot,
			.descriptorCount = 1,
			.descriptorType	 = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo	 = &info,
		};

		vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
	}

	void DescriptorHeap::reset_slot(const Context& ctx, u32 slot, HeapArray mask) noexcept
	{
		// Shutdown releases the fallbacks before the leak sweep runs; after that a
		// reset has nothing valid to write and the GPU is idle anyway.
		if (m_fallbacks.sampled_view == VK_NULL_HANDLE)
			return;

		if ((mask & HeapArray::Sampled) != HeapArray::None)
			write_sampled(ctx, slot, m_fallbacks.sampled_view, VK_IMAGE_LAYOUT_GENERAL);
		if ((mask & HeapArray::Storage) != HeapArray::None)
			write_storage(ctx, slot, m_fallbacks.storage_view);
		if ((mask & HeapArray::Sampler) != HeapArray::None)
			write_sampler(ctx, slot, m_fallbacks.sampler_vk);
		if ((mask & HeapArray::Buffer) != HeapArray::None)
			write_buffer(ctx, slot, m_fallbacks.buffer_vk, VK_WHOLE_SIZE);
	}
}
