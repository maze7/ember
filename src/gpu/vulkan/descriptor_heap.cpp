#include "ember/gpu/common.h"
#include <ember/gpu/buffer.h>
#include <ember/gpu/sampler.h>
#include <ember/gpu/texture.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/descriptor_heap.h>

#include <cstring>
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

		const VkDescriptorSetLayoutBinding bindings[] = {
			{BINDING_SAMPLED_2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, texture_capacity, VK_SHADER_STAGE_ALL, nullptr},
			{BINDING_SAMPLED_2D_ARRAY,
			 VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			 texture_capacity,
			 VK_SHADER_STAGE_ALL,
			 nullptr},
			{BINDING_SAMPLED_CUBE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, texture_capacity, VK_SHADER_STAGE_ALL, nullptr},
			{BINDING_SAMPLED_3D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, texture_capacity, VK_SHADER_STAGE_ALL, nullptr},
			{BINDING_STORAGE_IMAGES, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, texture_capacity, VK_SHADER_STAGE_ALL, nullptr},
			{BINDING_SAMPLERS, VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity, VK_SHADER_STAGE_ALL, nullptr},
			{BINDING_STORAGE_BUFFERS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_capacity, VK_SHADER_STAGE_ALL, nullptr},
		};

		const VkDescriptorBindingFlags binding_flags[] = {
			BINDLESS_FLAGS,
			BINDLESS_FLAGS,
			BINDLESS_FLAGS,
			BINDLESS_FLAGS,
			BINDLESS_FLAGS,
			BINDLESS_FLAGS,
			BINDLESS_FLAGS,
		};

		const VkDescriptorSetLayoutBindingFlagsCreateInfo heap_flags{
			.sType		   = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			.bindingCount  = static_cast<u32>(std::size(bindings)),
			.pBindingFlags = binding_flags,
		};

		const VkDescriptorSetLayoutCreateInfo heap_info{
			.sType		  = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext		  = &heap_flags,
			.flags		  = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			.bindingCount = static_cast<u32>(std::size(bindings)),
			.pBindings	  = bindings,
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
			{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, texture_capacity * SAMPLED_ARRAY_COUNT},
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

	void DescriptorHeap::write_sampled(
		const Context& ctx, u32 slot, VkImageView view, VkImageLayout layout, TextureType type) noexcept
	{
		EMBER_ASSERT(slot < m_texture_capacity);
		write_image(
			ctx,
			m_set,
			BINDING_SAMPLED_2D + static_cast<u32>(type),
			slot,
			VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			view,
			VK_NULL_HANDLE,
			layout);
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
		if (m_fallbacks.sampled_views[0] == VK_NULL_HANDLE)
			return;

		if ((mask & HeapArray::Sampled) != HeapArray::None)
		{
			for (u32 i = 0; i < SAMPLED_ARRAY_COUNT; ++i)
				write_image(
					ctx,
					m_set,
					BINDING_SAMPLED_2D + i,
					slot,
					VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
					m_fallbacks.sampled_views[i],
					VK_NULL_HANDLE,
					VK_IMAGE_LAYOUT_GENERAL);
		}

		if ((mask & HeapArray::Storage) != HeapArray::None)
			write_storage(ctx, slot, m_fallbacks.storage_view);
		if ((mask & HeapArray::Sampler) != HeapArray::None)
			write_sampler(ctx, slot, m_fallbacks.sampler_vk);
		if ((mask & HeapArray::Buffer) != HeapArray::None)
			write_buffer(ctx, slot, m_fallbacks.buffer_vk, VK_WHOLE_SIZE);
	}

	void DescriptorHeap::bind_fallbacks(const Context& ctx, const Fallbacks& fallbacks) noexcept
	{
		// Only the 2D fallback's pool index is load-bearing: reserving index 0
		// keeps any user texture from claiming slot 0 of any typed array. The
		// other fallbacks' slots are covered by the flood fill regardless.
		EMBER_ASSERT(fallbacks.texture[0].index == 0 && fallbacks.sampler.index == 0 && fallbacks.buffer.index == 0);
		EMBER_ASSERT(fallbacks.sampled_views[0] != VK_NULL_HANDLE && fallbacks.buffer_vk != VK_NULL_HANDLE);

		m_fallbacks = fallbacks;
		flood_fill(ctx);
	}

	void DescriptorHeap::flood_fill(const Context& ctx) noexcept
	{
		// Every slot of every array gets a fallback before the first user resource
		// exists. Chunked so boot never heap-allocates for this; runs once.
		constexpr u32 CHUNK = 256;

		VkDescriptorImageInfo images[CHUNK];

		const auto fill = [&](u32 binding, VkDescriptorType type, VkDescriptorImageInfo info, u32 count)
		{
			for (VkDescriptorImageInfo& entry : images)
				entry = info;

			for (u32 first = 0; first < count; first += CHUNK)
			{
				const VkWriteDescriptorSet write{
					.sType			 = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet			 = m_set,
					.dstBinding		 = binding,
					.dstArrayElement = first,
					.descriptorCount = std::min(CHUNK, count - first),
					.descriptorType	 = type,
					.pImageInfo		 = images,
				};

				vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
			}
		};

		// GENERAL everywhere: the fallbacks carry Storage usage, so one layout
		// serves all arrays and no per-array bookkeeping exists.
		for (u32 i = 0; i < SAMPLED_ARRAY_COUNT; ++i)
			fill(
				BINDING_SAMPLED_2D + i,
				VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				{VK_NULL_HANDLE, m_fallbacks.sampled_views[i], VK_IMAGE_LAYOUT_GENERAL},
				m_texture_capacity);

		fill(
			BINDING_STORAGE_IMAGES,
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			{VK_NULL_HANDLE, m_fallbacks.storage_view, VK_IMAGE_LAYOUT_GENERAL},
			m_texture_capacity);
		fill(
			BINDING_SAMPLERS,
			VK_DESCRIPTOR_TYPE_SAMPLER,
			{m_fallbacks.sampler_vk, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
			m_sampler_capacity);

		VkDescriptorBufferInfo buffers[CHUNK];
		for (VkDescriptorBufferInfo& entry : buffers)
			entry = {m_fallbacks.buffer_vk, 0, VK_WHOLE_SIZE};

		for (u32 first = 0; first < m_buffer_capacity; first += CHUNK)
		{
			const VkWriteDescriptorSet write{
				.sType			 = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet			 = m_set,
				.dstBinding		 = BINDING_STORAGE_BUFFERS,
				.dstArrayElement = first,
				.descriptorCount = std::min(CHUNK, m_buffer_capacity - first),
				.descriptorType	 = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo	 = buffers,
			};

			vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
		}
	}

	void DescriptorHeap::bind_constants(const Context& ctx, VkBuffer ring, u64 window_bytes) noexcept
	{
		VkDescriptorBufferInfo infos[CONSTANT_BUFFER_SLOTS];
		VkWriteDescriptorSet writes[CONSTANT_BUFFER_SLOTS];

		for (u32 i = 0; i < CONSTANT_BUFFER_SLOTS; ++i)
		{
			// Base offset 0 with a fixed window; dynamic offsets do the addressing.
			// The window is what a shader may read past the offset.
			infos[i]  = {ring, 0, window_bytes};
			writes[i] = {
				.sType			 = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet			 = m_constants,
				.dstBinding		 = i,
				.descriptorCount = 1,
				.descriptorType	 = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
				.pBufferInfo	 = &infos[i],
			};
		}

		vkUpdateDescriptorSets(ctx.device, CONSTANT_BUFFER_SLOTS, writes, 0, nullptr);
	}

	bool heap_boot(Device& device, Backend& backend) noexcept
	{
		DescriptorHeap& heap = backend.descriptor_heap;

		if (!heap.init(
				backend.context,
				backend.resources.textures.capacity(),
				backend.resources.samplers.capacity(),
				backend.resources.buffers.capacity()))
			return false;

		struct FallbackDef
		{
			const char* name;
			TextureType type;
			u32 layers;
		};

		constexpr FallbackDef FALLBACKS[] = {
			{"ember.fallback.2d", TextureType::Texture2D, 1},
			{"ember.fallback.2d_array", TextureType::Texture2DArray, 1},
			{"ember.fallback.cube", TextureType::TextureCube, 6},
			{"ember.fallback.3d", TextureType::Texture3D, 1},
		};

		// Magenta screams in development builds; white multiplies through as
		// identity in shipped ones.
		static constexpr u8 LOUD[4]	 = {0xff, 0x00, 0xff, 0xff};
		static constexpr u8 QUIET[4] = {0xff, 0xff, 0xff, 0xff};
		const u8* pixel				 = GPU_VALIDATION_DEFAULT ? LOUD : QUIET;

		DescriptorHeap::Fallbacks fb{};

		// Through the public API: the fallbacks exercise the same path as every
		// user resource, and as the first pool inserts the 2D one takes index 0.
		for (u32 i = 0; i < SAMPLED_ARRAY_COUNT; ++i)
		{
			u8 pixels[6 * 4];
			for (u32 layer = 0; layer < FALLBACKS[i].layers; ++layer)
				std::memcpy(pixels + layer * 4, pixel, 4);

			const TextureHandle handle = device.create_texture({
				.name		  = FALLBACKS[i].name,
				.type		  = FALLBACKS[i].type,
				.extent		  = {1, 1, 1},
				.layers		  = FALLBACKS[i].layers,
				.format		  = TextureFormat::RGBA8Unorm,
				.usage		  = TextureUsage::Sampled | TextureUsage::Storage,
				.initial_data = Span<const u8>{pixels, FALLBACKS[i].layers * 4u},
			});

			if (handle.is_null())
				return false;

			const TextureHot& hot = *backend.resources.textures.get(handle);
			fb.texture[i]		  = handle;
			fb.sampled_views[i]	  = hot.sampled_view;
		}

		fb.storage_view = backend.resources.textures.get(fb.texture[0])->storage_view;

		static constexpr u8 ZERO[256] = {};

		fb.buffer = device.create_buffer({
			.name		  = "ember.fallback.zero",
			.size		  = sizeof(ZERO),
			.usage		  = BufferUsage::Storage,
			.initial_data = Span<const u8>{ZERO, sizeof(ZERO)},
		});

		fb.sampler = device.create_sampler({.name = "ember.fallback.linear"});

		if (fb.buffer.is_null() || fb.sampler.is_null())
			return false;

		fb.buffer_vk  = backend.resources.buffers.get(fb.buffer)->handle;
		fb.sampler_vk = backend.resources.samplers.get(fb.sampler)->handle;

		heap.bind_fallbacks(backend.context, fb);
		return true;
	}

	void heap_destroy_fallbacks(Backend& backend) noexcept
	{
		const DescriptorHeap::Fallbacks& fb = backend.descriptor_heap.fallbacks();

		// Raw teardown on purpose: the GPU is idle, the sweep must not warn about
		// backend-owned entries, and the public destroy would trip the slot-0 assert.
		for (const TextureHandle handle : fb.texture)
		{
			if (const TextureHot* hot = backend.resources.textures.get(handle))
			{
				vkDestroyImageView(backend.context.device, hot->sampled_view, nullptr);
				vkDestroyImageView(backend.context.device, hot->storage_view, nullptr);
				vmaDestroyImage(
					backend.context.allocator, hot->image, backend.resources.textures.get_cold(handle)->allocation);
				(void)backend.resources.textures.erase(handle);
			}
		}

		if (const BufferHot* hot = backend.resources.buffers.get(fb.buffer))
		{
			vmaDestroyBuffer(
				backend.context.allocator, hot->handle, backend.resources.buffers.get_cold(fb.buffer)->allocation);
			(void)backend.resources.buffers.erase(fb.buffer);
		}

		if (const SamplerData* data = backend.resources.samplers.get(fb.sampler))
		{
			vkDestroySampler(backend.context.device, data->handle, nullptr);
			(void)backend.resources.samplers.erase(fb.sampler);
		}

		backend.descriptor_heap.clear_fallbacks();
	}
}
