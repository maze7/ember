#pragma once

#include "ember/core/bitmask.h"
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <gpu/vulkan/common.h>
#include <vulkan/vulkan_core.h>

namespace ember::gpu
{
	struct Backend;
}

namespace ember::gpu::vk
{
	/// Which arrays a deferred slot reset touches (DestroyQueue::reset_slot mask)
	enum class HeapArray : u8
	{
		Sampled = 1 << 0,
		Storage = 1 << 1,
		Sampler = 1 << 1,
		Buffer	= 1 << 3,
	};

	EMBER_ENUM_BITWISE_OPS(HeapArray, u8);

	/**
	 * The bindless heap: one update-after-bind set with four fixed arrays, the
	 * dynamic-constant set, and the pipeline layout every pipeline compiles against.
	 *
	 * A resource's descriptor lives at its handle's Pool index.
	 * Creation writes the slot immediately; destruction resets it to the fallback through
	 * the DestroyQueue, so the descriptor outlives every frame that could read it.
	 *
	 * Every slot always holds a valid descriptor (boot flood-fills with fallbacks), so a
	 * stale or GPU-generated garbage index reads as a white/zero instead of crashing.
	 *
	 * The set and layout handles are private so nothing can write around the discipline.
	 */
	class DescriptorHeap
	{
	public:
		/// The slot-0 residents plus their raw handles, cached so drain-time resets
		/// never touch the pools.
		struct Fallbacks
		{
			TextureHandle texture{};
			SamplerHandle sampler{};
			BufferHandle buffer{};
			VkImageView sampled_view = VK_NULL_HANDLE;
			VkImageView storage_view = VK_NULL_HANDLE;
			VkSampler sampler_vk	 = VK_NULL_HANDLE;
			VkBuffer buffer_vk		 = VK_NULL_HANDLE;
		};

		/// Layouts, pool, both sets, and the update-after-bind limit tripwire.
		[[nodiscard]] bool
		init(const Context& ctx, u32 texture_capacity, u32 sampler_capacity, u32 buffer_capacity) noexcept;

		/// Caches the slot-0 residents and flood-fills every slot of every array.
		/// Asserts the fallbacks took index 0: the null handle aliases slot 0.
		void bind_fallbacks(const Context& ctx, const Fallbacks& fallbacks) noexcept;

		/// Set-1 descriptors point at the transient ring, written once; per-draw variation
		/// is dynamic offsets only. Called by transient_boot.
		void bind_constants(const Context& ctx, VkBuffer ring, u64 window_bytes) noexcept;

		/// Shutdown only, after the fallback pool entries are destroyed: makes every later
		/// reset_slot a no-op instead of a write of dead handles.
		void clear_fallbacks() noexcept { m_fallbacks = {}; }

		/// Native teardown (the pool frees both sets).
		void destroy(const Context& ctx) noexcept;

		// Creation-time writes. `layout` is the texture's steady layout: a
		// descriptor must state the layout the image is in when a shader reads it.
		void write_sampled(const Context& ctx, u32 slot, VkImageView view, VkImageLayout layout) noexcept;
		void write_storage(const Context& ctx, u32 slot, VkImageView view) noexcept;
		void write_sampler(const Context& ctx, u32 slot, VkSampler sampler) noexcept;
		void write_buffer(const Context& ctx, u32 slot, VkBuffer buffer, u64 size) noexcept;

		/// Drain-time: point `slot` back at the fallbacks in every array `mask` names.
		/// No-ops once the fallbacks are released (shutdown).
		void reset_slot(const Context& ctx, u32 slot, u8 mask) noexcept;

		/// Every pipeline is compiled against this.
		[[nodiscard]] VkPipelineLayout pipeline_layout() const noexcept { return m_pipeline_layout; }
		[[nodiscard]] VkDescriptorSet heap_set() const noexcept { return m_set; }
		[[nodiscard]] VkDescriptorSet constants_set() const noexcept { return m_constants; }

	private:
		[[nodiscard]] bool check_limits(const Context& ctx) const noexcept;
		void flood_fill(const Context& ctx) noexcept;

		VkDescriptorSetLayout m_heap_layout		 = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_constants_layout = VK_NULL_HANDLE;
		VkPipelineLayout m_pipeline_layout		 = VK_NULL_HANDLE;
		VkDescriptorPool m_pool					 = VK_NULL_HANDLE;
		VkDescriptorSet m_set					 = VK_NULL_HANDLE;
		VkDescriptorSet m_constants				 = VK_NULL_HANDLE;

		Fallbacks m_fallbacks{};

		u32 m_texture_capacity = 0;
		u32 m_sampler_capacity = 0;
		u32 m_buffer_capacity  = 0;
	};
}
