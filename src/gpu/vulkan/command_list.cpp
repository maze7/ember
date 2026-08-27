#include "ember/core/common.h"
#include "ember/gpu/common.h"
#include "ember/memory/common.h"
#include <ember/gpu/command_list.h>
#include <ember/gpu/device.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/formats.h>

namespace ember::gpu
{
	namespace
	{
		struct StateInfo
		{
			VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 access		 = VK_ACCESS_2_NONE;
			VkImageLayout layout		 = VK_IMAGE_LAYOUT_UNDEFINED;
		};

		[[nodiscard]] constexpr StateInfo state_info(TextureState state) noexcept
		{
			switch (state)
			{
				case TextureState::Undefined:
					// Source stage pins to COLOR_ATTACHMENT_OUTPUT so a backbuffer's first
					// transition orders after the acquire semaphore, whose wait submit_frame
					// scopes to that stage. Costs nothing for other first uses.
					return {
						VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};

				case TextureState::RenderTarget:
					return {
						VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
						VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
						VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

				case TextureState::DepthTarget:
					return {
						VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
						VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
						VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

				case TextureState::ShaderRead:
					return {
						VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
						VK_ACCESS_2_SHADER_READ_BIT,
						VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

				case TextureState::CopySrc:
					return {
						VK_PIPELINE_STAGE_2_COPY_BIT,
						VK_ACCESS_2_TRANSFER_READ_BIT,
						VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};

				case TextureState::CopyDst:
					return {
						VK_PIPELINE_STAGE_2_COPY_BIT,
						VK_ACCESS_2_TRANSFER_WRITE_BIT,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};

				case TextureState::Present:
					// No destination work; the present semaphore takes over.
					return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
			}

			EMBER_UNREACHABLE_ASSERT();
		}

		[[nodiscard]] VkCommandBuffer recording_cmd(Backend& backend) noexcept
		{
			EMBER_ASSERT(backend.frame.list_open && "CommandList used outside begin_comand_list/submit");
			const u32 slot = static_cast<u32>(backend.frame.index % backend.context.frames_in_flight);
			return backend.frame.slots[slot].commands;
		}
	}

	void CommandList::barrier(Span<const TextureBarrier> barriers) noexcept
	{
		if (m_backend == nullptr)
			return;

		const VkCommandBuffer cmd = recording_cmd(*m_backend);

		constexpr u32 BATCH = 8;
		VkImageMemoryBarrier2 native[BATCH];
		u32 count = 0;

		for (const TextureBarrier& barrier : barriers)
		{
			EMBER_ASSERT(barrier.after != TextureState::Undefined && "cannot transition into garbage");

			const vk::TextureHot* hot = m_backend->resources.textures.try_get(barrier.texture);
			if (hot == nullptr)
			{
				EMBER_ASSERT(false && "barrier on a stale texture handle");
				continue;
			}

			const vk::TextureCold& cold = m_backend->resources.textures.get_cold(barrier.texture);
			const StateInfo src			= state_info(barrier.before);
			const StateInfo dst			= state_info(barrier.after);

			native[count++] = {
				.sType		   = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask  = src.stages,
				.srcAccessMask = src.access,
				.dstStageMask  = dst.stages,
				.dstAccessMask = dst.access,
				.oldLayout	   = src.layout,
				.newLayout	   = dst.layout,
				.image		   = hot->image,
				.subresourceRange =
					{vk::format_info(cold.api_format).aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
			};

			if (count == BATCH)
			{
				const VkDependencyInfo dependency{
					.sType					 = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					.imageMemoryBarrierCount = count,
					.pImageMemoryBarriers	 = native,
				};
				vkCmdPipelineBarrier2(cmd, &dependency);
				count = 0;
			}
		}

		if (count > 0)
		{
			const VkDependencyInfo dependency{
				.sType					 = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = count,
				.pImageMemoryBarriers	 = native,
			};
			vkCmdPipelineBarrier2(cmd, &dependency);
		}
	}

	void CommandList::begin_rendering(const RenderingDef& def) noexcept
	{
		if (m_backend == nullptr)
			return;

		const VkCommandBuffer cmd = recording_cmd(*m_backend);
		EMBER_ASSERT(!def.colors.empty() || !def.depth.texture.is_null());

		auto to_vk_load = [](LoadOp op) noexcept
		{
			switch (op)
			{
				case LoadOp::Load:
					return VK_ATTACHMENT_LOAD_OP_LOAD;
				case LoadOp::Clear:
					return VK_ATTACHMENT_LOAD_OP_CLEAR;
				case LoadOp::DontCare:
					return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			}
			return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		};

		auto to_vk_store = [](StoreOp op) noexcept
		{ return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE; };

		VkRenderingAttachmentInfo colors[MAX_COLOR_ATTACHMENTS];
		VkExtent3D extent{};

		for (u32 i = 0; i < def.colors.size(); ++i)
		{
			const ColorAttachment& attachment = def.colors[i];
			const vk::TextureHot& hot		  = m_backend->resources.textures.get(attachment.texture);
			extent							  = m_backend->resources.textures.get_cold(attachment.texture).extent;

			colors[i] = {
				.sType		 = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView	 = hot.sampled_view,
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.loadOp		 = to_vk_load(attachment.load),
				.storeOp	 = to_vk_store(attachment.store),
				.clearValue =
					{.color =
						 {.float32 = {attachment.clear.r, attachment.clear.g, attachment.clear.b, attachment.clear.a}}},
			};
		}

		VkRenderingAttachmentInfo depth{};
		const bool has_depth = !def.depth.texture.is_null();

		if (has_depth)
		{
			const vk::TextureHot& hot = m_backend->resources.textures.get(def.depth.texture);
			extent					  = m_backend->resources.textures.get_cold(def.depth.texture).extent;

			depth = {
				.sType		 = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView	 = hot.sampled_view,
				.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				.loadOp		 = to_vk_load(def.depth.load),
				.storeOp	 = to_vk_store(def.depth.store),
				.clearValue	 = {.depthStencil = {def.depth.clear_depth, 0}},
			};
		}

		const VkRenderingInfo rendering{
			.sType				  = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea			  = {{0, 0}, {extent.width, extent.height}},
			.layerCount			  = 1,
			.colorAttachmentCount = static_cast<u32>(def.colors.size()),
			.pColorAttachments	  = colors,
			.pDepthAttachment	  = has_depth ? &depth : nullptr,
		};

		vkCmdBeginRendering(cmd, &rendering);

		// Negative height flips clip space to Y up, matching D3D12, so meshes, winding
		// and projection math stay identical across backends.
		const VkViewport viewport{
			.x		  = 0.0f,
			.y		  = static_cast<f32>(extent.height),
			.width	  = static_cast<f32>(extent.width),
			.height	  = -static_cast<f32>(extent.height),
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};
		vkCmdSetViewport(cmd, 0, 1, &viewport);

		const VkRect2D scissor{{0, 0}, {extent.width, extent.height}};
		vkCmdSetScissor(cmd, 0, 1, &scissor);
	}

	void CommandList::end_rendering() noexcept
	{
		if (m_backend == nullptr)
			return;

		vkCmdEndRendering(recording_cmd(*m_backend));
	}

	void CommandList::set_pipeline(GraphicsPipelineHandle pipeline) noexcept
	{
		if (m_backend == nullptr)
			return;

		const vk::PipelineData* data = m_backend->resources.graphics_pipelines.try_get(pipeline);
		if (data == nullptr)
		{
			EMBER_ASSERT(false && "set_pipeline on a stale handle");
			return;
		}

		vkCmdBindPipeline(recording_cmd(*m_backend), VK_PIPELINE_BIND_POINT_GRAPHICS, data->pipeline);
	}

	void CommandList::push(const void* data, u32 size) noexcept
	{
		if (m_backend == nullptr)
			return;

		// Ember uses one layout engine-wide, so pushing never depends on which pipeline is bound.
		vkCmdPushConstants(
			recording_cmd(*m_backend),
			m_backend->descriptor_heap.pipeline_layout(),
			VK_SHADER_STAGE_ALL,
			0,
			size,
			data);
	}

	void CommandList::draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) noexcept
	{
		if (m_backend == nullptr)
			return;

		vkCmdDraw(recording_cmd(*m_backend), vertex_count, instance_count, first_vertex, first_instance);
	}
}
