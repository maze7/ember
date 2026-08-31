#include <ember/core/common.h>
#include <ember/gpu/command_list.h>
#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/formats.h>
#include <vulkan/vulkan_core.h>

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

		[[nodiscard]] StateInfo state_info(TextureState state, VkPipelineStageFlags2 shader_stages) noexcept
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
					return {shader_stages, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

				case TextureState::ShaderWrite:
					return {
						shader_stages,
						VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
						VK_IMAGE_LAYOUT_GENERAL};

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

		struct BufferStateInfo
		{
			VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 access		 = VK_ACCESS_2_NONE;
		};

		[[nodiscard]] BufferStateInfo buffer_state_info(BufferState state, VkPipelineStageFlags2 shader_stages) noexcept
		{
			switch (state)
			{
				case BufferState::ShaderRead:
					return {shader_stages, VK_ACCESS_2_SHADER_READ_BIT};
				case BufferState::ShaderWrite:
					return {shader_stages, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
				case BufferState::IndexBuffer:
					return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT};
				case BufferState::IndirectArgument:
					return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
				case BufferState::CopySrc:
					return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
				case BufferState::CopyDst:
					return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
			}

			EMBER_UNREACHABLE_ASSERT();
		}

		[[nodiscard]] VkCommandBuffer recording_cmd(Backend& backend) noexcept
		{
			EMBER_ASSERT(backend.frame.list_open && "CommandList used outside begin_comand_list/submit");
			const u32 slot = static_cast<u32>(backend.frame.index % backend.context.frames_in_flight);
			return backend.frame.slots[slot].recording.commands;
		}

		/// Weak resolve for recording paths: a stale handle logs and skips the command
		/// instead of handing the driver a dead object.
		[[nodiscard]] VkBuffer resolve_buffer(Backend& backend, BufferHandle handle) noexcept
		{
			if (const vk::BufferHot* hot = backend.resources.buffers.get(handle))
				return hot->handle;

			EMBER_ERROR("gpu: command references a stale buffer handle");
			return VK_NULL_HANDLE;
		}

		void flush_graphics(Backend& backend, Recording& recording) noexcept
		{
			if (!recording.constants_dirty_graphics)
				return;

			const VkDescriptorSet set = backend.descriptor_heap.constants_set();

			vkCmdBindDescriptorSets(
				recording.commands,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				backend.descriptor_heap.pipeline_layout(),
				1,
				1,
				&set,
				CONSTANT_BUFFER_SLOTS,
				recording.constant_offsets);

			recording.constants_dirty_graphics = false;
		}

		void flush_compute(Backend& backend, Recording& recording) noexcept
		{
			if (!recording.constants_dirty_compute)
				return;

			const VkDescriptorSet set = backend.descriptor_heap.constants_set();

			vkCmdBindDescriptorSets(
				recording.commands,
				VK_PIPELINE_BIND_POINT_COMPUTE,
				backend.descriptor_heap.pipeline_layout(),
				1,
				1,
				&set,
				CONSTANT_BUFFER_SLOTS,
				recording.constant_offsets);

			recording.constants_dirty_compute = false;
		}

		void flush_index(Recording& recording) noexcept
		{
			if (!recording.index_dirty)
				return;

			vkCmdBindIndexBuffer(
				recording.commands, recording.index_buffer, recording.index_offset, recording.index_type);
			recording.index_dirty = false;
		}

		/// The public contract is top left origin with positive height on every
		/// backend. The negative height here is what makes clip space Y up over
		/// Vulkan; no caller ever sees the flip.
		[[nodiscard]] VkViewport flip_viewport(const Viewport& viewport) noexcept
		{
			return {
				.x		  = viewport.x,
				.y		  = viewport.y + viewport.height,
				.width	  = viewport.width,
				.height	  = -viewport.height,
				.minDepth = viewport.min_depth,
				.maxDepth = viewport.max_depth,
			};
		}
	}

	void CommandList::barrier(Span<const TextureBarrier> textures, Span<const BufferBarrier> buffers) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(!m_recording->inside_pass && "barriers record outside render passes");

		const VkCommandBuffer cmd				  = m_recording->commands;
		const VkPipelineStageFlags2 shader_stages = m_backend->context.all_shader_stages;

		constexpr u32 BATCH = 8;
		VkImageMemoryBarrier2 images[BATCH];
		VkBufferMemoryBarrier2 native_buffers[BATCH];
		u32 image_count	 = 0;
		u32 buffer_count = 0;

		const auto flush = [&]
		{
			const VkDependencyInfo dependency{
				.sType					  = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.bufferMemoryBarrierCount = buffer_count,
				.pBufferMemoryBarriers	  = native_buffers,
				.imageMemoryBarrierCount  = image_count,
				.pImageMemoryBarriers	  = images,
			};
			vkCmdPipelineBarrier2(cmd, &dependency);
			image_count	 = 0;
			buffer_count = 0;
		};

		for (const TextureBarrier& barrier : textures)
		{
			EMBER_ASSERT(barrier.after != TextureState::Undefined && "cannot transition into garbage");

			const vk::TextureHot* hot = m_backend->resources.textures.get(barrier.texture);
			if (hot == nullptr)
			{
				EMBER_ERROR("gpu: barrier references a stale texture handle");
				continue;
			}

			const vk::TextureCold& cold = *m_backend->resources.textures.get_cold(barrier.texture);
			const StateInfo src			= state_info(barrier.before, shader_stages);
			const StateInfo dst			= state_info(barrier.after, shader_stages);

			images[image_count++] = {
				.sType		   = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask  = src.stages,
				.srcAccessMask = src.access,
				.dstStageMask  = dst.stages,
				.dstAccessMask = dst.access,
				.oldLayout	   = src.layout,
				.newLayout	   = dst.layout,
				.image		   = hot->image,
				.subresourceRange =
					{
						vk::format_info(cold.api_format).aspect,
						barrier.base_mip,
						barrier.mip_count == ALL_MIPS ? VK_REMAINING_MIP_LEVELS : barrier.mip_count,
						barrier.base_layer,
						barrier.layer_count == ALL_LAYERS ? VK_REMAINING_ARRAY_LAYERS : barrier.layer_count,
					},
			};

			if (image_count == BATCH)
				flush();
		}

		for (const BufferBarrier& barrier : buffers)
		{
			const VkBuffer buffer = resolve_buffer(*m_backend, barrier.buffer);
			if (buffer == VK_NULL_HANDLE)
				continue;

			const BufferStateInfo src = buffer_state_info(barrier.before, shader_stages);
			const BufferStateInfo dst = buffer_state_info(barrier.after, shader_stages);

			native_buffers[buffer_count++] = {
				.sType		   = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				.srcStageMask  = src.stages,
				.srcAccessMask = src.access,
				.dstStageMask  = dst.stages,
				.dstAccessMask = dst.access,
				.buffer		   = buffer,
				.offset		   = 0,
				.size		   = VK_WHOLE_SIZE,
			};

			if (buffer_count == BATCH)
				flush();
		}

		if (image_count > 0 || buffer_count > 0)
			flush();
	}

	void CommandList::memory_barrier() noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(!m_recording->inside_pass && "barriers record outside render passes");

		const VkPipelineStageFlags2 shader_stages = m_backend->context.all_shader_stages;

		const VkMemoryBarrier2 barrier{
			.sType		   = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask  = shader_stages,
			.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
			.dstStageMask  = shader_stages,
			.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
		};

		const VkDependencyInfo dependency{
			.sType				= VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers	= &barrier,
		};

		vkCmdPipelineBarrier2(m_recording->commands, &dependency);
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
			const vk::TextureHot& hot		  = *m_backend->resources.textures.get(attachment.texture);
			extent							  = m_backend->resources.textures.get_cold(attachment.texture)->extent;

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
			const vk::TextureHot& hot = *m_backend->resources.textures.get(def.depth.texture);
			extent					  = m_backend->resources.textures.get_cold(def.depth.texture)->extent;

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

		const VkViewport viewport = flip_viewport({
			.width	= static_cast<f32>(extent.width),
			.height = static_cast<f32>(extent.height),
		});
		vkCmdSetViewport(cmd, 0, 1, &viewport);

		const VkRect2D scissor{{0, 0}, {extent.width, extent.height}};
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		m_recording->inside_pass = true;
	}

	void CommandList::end_rendering() noexcept
	{
		if (m_backend == nullptr)
			return;

		vkCmdEndRendering(recording_cmd(*m_backend));
		m_recording->inside_pass = false;
	}

	void CommandList::set_viewport(const Viewport& viewport) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_recording->inside_pass && "viewport overrides apply inside a render pass");
		EMBER_ASSERT(viewport.width > 0.0f && viewport.height > 0.0f);

		const VkViewport native = flip_viewport(viewport);
		vkCmdSetViewport(m_recording->commands, 0, 1, &native);
	}

	void CommandList::set_scissor(const Rect2D& scissor) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_recording->inside_pass && "scissor overrides apply inside a render pass");
		EMBER_ASSERT(scissor.x >= 0 && scissor.y >= 0);

		// Scissor rects live in framebuffer coordinates on every backend; the
		// viewport flip does not touch them.
		const VkRect2D native{
			.offset = {scissor.x, scissor.y},
			.extent = {scissor.width, scissor.height},
		};

		vkCmdSetScissor(m_recording->commands, 0, 1, &native);
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

	void CommandList::set_pipeline(GraphicsPipelineHandle pipeline) noexcept
	{
		if (m_backend == nullptr || m_recording->graphics_pipeline == pipeline)
			return;

		const vk::PipelineData* data = m_backend->resources.graphics_pipelines.get(pipeline);
		if (data == nullptr)
		{
			EMBER_ERROR("gpu: set_pipeline on a stale graphics handle");
			return;
		}

		vkCmdBindPipeline(m_recording->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, data->pipeline);
		m_recording->graphics_pipeline = pipeline;
	}

	void CommandList::set_pipeline(ComputePipelineHandle pipeline) noexcept
	{
		if (m_backend == nullptr || m_recording->compute_pipeline == pipeline)
			return;

		const vk::PipelineData* data = m_backend->resources.compute_pipelines.get(pipeline);
		if (data == nullptr)
		{
			EMBER_ERROR("gpu: set_pipeline on a stale compute handle");
			return;
		}

		vkCmdBindPipeline(m_recording->commands, VK_PIPELINE_BIND_POINT_COMPUTE, data->pipeline);
		m_recording->compute_pipeline = pipeline;
	}

	void CommandList::set_index_buffer(BufferHandle buffer, IndexFormat format, u64 offset) noexcept
	{
		if (m_backend == nullptr)
			return;

		const VkBuffer native = resolve_buffer(*m_backend, buffer);
		if (native == VK_NULL_HANDLE)
			return;

		Recording& recording   = *m_recording;
		recording.index_buffer = native;
		recording.index_offset = offset;
		recording.index_type   = format == IndexFormat::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
		recording.index_dirty  = true;
	}

	void CommandList::draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_recording->inside_pass && "draws record inside a render pass");

		flush_graphics(*m_backend, *m_recording);
		vkCmdDraw(m_recording->commands, vertex_count, instance_count, first_vertex, first_instance);
	}

	void CommandList::draw_indexed(
		u32 index_count, u32 instance_count, u32 first_index, i32 base_vertex, u32 first_instance) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_recording->inside_pass && "draws record inside a render pass");
		EMBER_ASSERT(m_recording->index_buffer != VK_NULL_HANDLE && "draw_indexed without set_index_buffer");

		flush_graphics(*m_backend, *m_recording);
		flush_index(*m_recording);
		vkCmdDrawIndexed(m_recording->commands, index_count, instance_count, first_index, base_vertex, first_instance);
	}

	void CommandList::draw_indirect(BufferHandle args, u64 offset, u32 draw_count, u32 stride) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_recording->inside_pass && "draws record inside a render pass");
		EMBER_ASSERT(stride >= sizeof(DrawIndirectArgs) && stride % 4 == 0);

		const VkBuffer native = resolve_buffer(*m_backend, args);
		if (native == VK_NULL_HANDLE)
			return;

		flush_graphics(*m_backend, *m_recording);
		vkCmdDrawIndirect(m_recording->commands, native, offset, draw_count, stride);
	}

	void CommandList::draw_indexed_indirect(BufferHandle args, u64 offset, u32 draw_count, u32 stride) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(m_recording->inside_pass && "draws record inside a render pass");
		EMBER_ASSERT(m_recording->index_buffer != VK_NULL_HANDLE && "indexed draw without set_index_buffer");
		EMBER_ASSERT(stride >= sizeof(DrawIndexedIndirectArgs) && stride % 4 == 0);

		const VkBuffer native = resolve_buffer(*m_backend, args);
		if (native == VK_NULL_HANDLE)
			return;

		flush_graphics(*m_backend, *m_recording);
		flush_index(*m_recording);
		vkCmdDrawIndexedIndirect(m_recording->commands, native, offset, draw_count, stride);
	}

	void CommandList::draw_indirect_count(
		BufferHandle args, u64 offset, BufferHandle count, u64 count_offset, u32 max_draw_count, u32 stride) noexcept
	{
		if (m_backend == nullptr)
			return;

		if (!m_backend->context.caps.indirect_count)
		{
			EMBER_ERROR("gpu: draw_indirect_count needs caps.indirect_count");
			return;
		}

		EMBER_ASSERT(m_recording->inside_pass && "draws record inside a render pass");
		EMBER_ASSERT(stride >= sizeof(DrawIndirectArgs) && stride % 4 == 0);

		const VkBuffer native_args	= resolve_buffer(*m_backend, args);
		const VkBuffer native_count = resolve_buffer(*m_backend, count);
		if (native_args == VK_NULL_HANDLE || native_count == VK_NULL_HANDLE)
			return;

		flush_graphics(*m_backend, *m_recording);
		vkCmdDrawIndirectCount(
			m_recording->commands, native_args, offset, native_count, count_offset, max_draw_count, stride);
	}

	void CommandList::draw_indexed_indirect_count(
		BufferHandle args, u64 offset, BufferHandle count, u64 count_offset, u32 max_draw_count, u32 stride) noexcept
	{
		if (m_backend == nullptr)
			return;

		if (!m_backend->context.caps.indirect_count)
		{
			EMBER_ERROR("gpu: draw_indexed_indirect_count needs caps.indirect_count");
			return;
		}

		EMBER_ASSERT(m_recording->inside_pass && "draws record inside a render pass");
		EMBER_ASSERT(m_recording->index_buffer != VK_NULL_HANDLE && "indexed draw without set_index_buffer");
		EMBER_ASSERT(stride >= sizeof(DrawIndexedIndirectArgs) && stride % 4 == 0);

		const VkBuffer native_args	= resolve_buffer(*m_backend, args);
		const VkBuffer native_count = resolve_buffer(*m_backend, count);
		if (native_args == VK_NULL_HANDLE || native_count == VK_NULL_HANDLE)
			return;

		flush_graphics(*m_backend, *m_recording);
		flush_index(*m_recording);
		vkCmdDrawIndexedIndirectCount(
			m_recording->commands, native_args, offset, native_count, count_offset, max_draw_count, stride);
	}

	void CommandList::dispatch(u32 x, u32 y, u32 z) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(!m_recording->inside_pass && "dispatch records outside render passes");

		flush_compute(*m_backend, *m_recording);
		vkCmdDispatch(m_recording->commands, x, y, z);
	}

	void CommandList::dispatch_indirect(BufferHandle args, u64 offset) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(!m_recording->inside_pass && "dispatch records outside render passes");

		const VkBuffer native = resolve_buffer(*m_backend, args);
		if (native == VK_NULL_HANDLE)
			return;

		flush_compute(*m_backend, *m_recording);
		vkCmdDispatchIndirect(m_recording->commands, native, offset);
	}

	void CommandList::copy_buffer(BufferHandle src, u64 src_offset, BufferHandle dst, u64 dst_offset, u64 size) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(!m_recording->inside_pass && "copies record outside render passes");

		const VkBuffer native_src = resolve_buffer(*m_backend, src);
		const VkBuffer native_dst = resolve_buffer(*m_backend, dst);
		if (native_src == VK_NULL_HANDLE || native_dst == VK_NULL_HANDLE)
			return;

		const VkBufferCopy2 region{
			.sType	   = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
			.srcOffset = src_offset,
			.dstOffset = dst_offset,
			.size	   = size,
		};

		const VkCopyBufferInfo2 copy{
			.sType		 = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
			.srcBuffer	 = native_src,
			.dstBuffer	 = native_dst,
			.regionCount = 1,
			.pRegions	 = &region,
		};

		vkCmdCopyBuffer2(m_recording->commands, &copy);
	}

	void CommandList::fill_buffer(BufferHandle dst, u64 offset, u64 size, u32 value) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(!m_recording->inside_pass && "copies record outside render passes");
		EMBER_ASSERT(offset % 4 == 0 && size % 4 == 0);

		const VkBuffer native = resolve_buffer(*m_backend, dst);
		if (native == VK_NULL_HANDLE)
			return;

		vkCmdFillBuffer(m_recording->commands, native, offset, size, value);
	}

	void CommandList::set_constants_raw(u32 slot, const void* data, u32 size) noexcept
	{
		if (m_backend == nullptr)
			return;

		EMBER_ASSERT(slot < CONSTANT_BUFFER_SLOTS);

		Backend& backend = *m_backend;

		const TransientAllocation allocation =
			backend.transient.allocate(size, backend.context.caps.constant_buffer_offset_alignment);

		if (!allocation.valid())
		{
			EMBER_ERROR("gpu: constant allocation failed; slot {} keeps its previous data", slot);
			return;
		}

		// Set 1's descriptors address the primary ring and nothing else, so an
		// allocation that spilled to an overflow page cannot be bound as constants.
		if (allocation.buffer != backend.transient_ring.handle)
		{
			EMBER_ERROR("gpu: constants landed on an overflow page; raise DeviceDef::transient_ring_bytes");
			return;
		}

		std::memcpy(allocation.cpu, data, size);

		m_recording->constant_offsets[slot]	  = allocation.offset;
		m_recording->constants_dirty_graphics = true;
		m_recording->constants_dirty_compute  = true;
	}
}
