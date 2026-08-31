#include <ember/gpu/device.h>
#include <ember/gpu/pipeline.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/common.h>
#include <gpu/vulkan/formats.h>

namespace ember::gpu
{
	namespace
	{
		[[nodiscard]] VkPrimitiveTopology to_vk_topology(PrimitiveTopology topology) noexcept
		{
			switch (topology)
			{
				case PrimitiveTopology::TriangleList:
					return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				case PrimitiveTopology::TriangleStrip:
					return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
				case PrimitiveTopology::LineList:
					return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
				case PrimitiveTopology::PointList:
					return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			}
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		}

		[[nodiscard]] VkCullModeFlags to_vk_cull(CullMode cull) noexcept
		{
			switch (cull)
			{
				case CullMode::None:
					return VK_CULL_MODE_NONE;
				case CullMode::Back:
					return VK_CULL_MODE_BACK_BIT;
				case CullMode::Front:
					return VK_CULL_MODE_FRONT_BIT;
			}
			return VK_CULL_MODE_NONE;
		}

		[[nodiscard]] VkPipelineColorBlendAttachmentState to_vk_blend(BlendPreset preset) noexcept
		{
			VkPipelineColorBlendAttachmentState state{
				.blendEnable	= VK_FALSE,
				.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
								  VK_COLOR_COMPONENT_A_BIT,
			};

			switch (preset)
			{
				case BlendPreset::Opaque:
					break;

				case BlendPreset::AlphaBlend:
					state.blendEnable		  = VK_TRUE;
					state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
					state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					state.colorBlendOp		  = VK_BLEND_OP_ADD;
					state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
					state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					state.alphaBlendOp		  = VK_BLEND_OP_ADD;
					break;

				case BlendPreset::Additive:
					state.blendEnable		  = VK_TRUE;
					state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
					state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
					state.colorBlendOp		  = VK_BLEND_OP_ADD;
					state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
					state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
					state.alphaBlendOp		  = VK_BLEND_OP_ADD;
					break;

				case BlendPreset::PremultipliedAlpha:
					state.blendEnable		  = VK_TRUE;
					state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
					state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					state.colorBlendOp		  = VK_BLEND_OP_ADD;
					state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
					state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					state.alphaBlendOp		  = VK_BLEND_OP_ADD;
					break;
			}

			return state;
		}

		[[nodiscard]] VkShaderModule create_module(VkDevice device, Span<const u8> code) noexcept
		{
			// SPIR-V is a u32 stream; a misaligned blob is UB at the reinterpret below.
			if ((reinterpret_cast<uintptr_t>(code.data()) & 3) != 0)
			{
				EMBER_ERROR("gpu: shader bytecode is not 4-byte aligned");
				return VK_NULL_HANDLE;
			}
			const VkShaderModuleCreateInfo info{
				.sType	  = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
				.codeSize = code.size(),
				.pCode	  = reinterpret_cast<const u32*>(code.data()),
			};

			VkShaderModule module = VK_NULL_HANDLE;
			if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
				return VK_NULL_HANDLE;

			return module;
		}
	}

	GraphicsPipelineHandle Device::create_graphics_pipeline(const GraphicsPipelineDef& def) noexcept
	{
		EMBER_GPU_GUARD({});

		if (!::ember::gpu::is_valid(def))
		{
			EMBER_ERROR("gpu: pipeline '{}' has an invalid def", def.name);
			return {};
		}

		if (def.fill == FillMode::Wireframe && !m_backend->context.caps.wireframe)
		{
			EMBER_ERROR("gpu: pipeline '{}' wants wireframe; the adapter has no fill mode support", def.name);
			return {};
		}

		const VkDevice device = m_backend->context.device;

		const VkShaderModule vertex	  = create_module(device, def.vertex.code);
		const VkShaderModule fragment = create_module(device, def.fragment.code);

		if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE)
		{
			EMBER_ERROR("gpu: pipeline '{}' shader module creation failed", def.name);
			vkDestroyShaderModule(device, vertex, nullptr);
			vkDestroyShaderModule(device, fragment, nullptr);
			return {};
		}

		const VkPipelineShaderStageCreateInfo stages[] = {
			{
				.sType	= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage	= VK_SHADER_STAGE_VERTEX_BIT,
				.module = vertex,
				.pName	= def.vertex.entry,
			},
			{
				.sType	= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage	= VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = fragment,
				.pName	= def.fragment.entry,
			},
		};

		// Empty on purpose, forever: vertices are pulled from buffer addresses in shaders.
		const VkPipelineVertexInputStateCreateInfo vertex_input{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		};

		const VkPipelineInputAssemblyStateCreateInfo input_assembly{
			.sType	  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = to_vk_topology(def.topology),
		};

		// Counts only; the values are dynamic state, set by begin_rendering.
		const VkPipelineViewportStateCreateInfo viewport{
			.sType		   = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount  = 1,
		};

		const VkPipelineRasterizationStateCreateInfo raster{
			.sType		 = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = def.fill == FillMode::Wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
			.cullMode	 = to_vk_cull(def.cull),
			// The negative viewport flip mirrors framebuffer winding, so the enum speaks
			// the content convention (glTF: CCW outward, Y up) and the backend compensates.
			.frontFace =
				def.front == FrontFace::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
			.lineWidth = 1.0f,
		};

		const VkPipelineMultisampleStateCreateInfo multisample{
			.sType				  = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};

		const VkPipelineDepthStencilStateCreateInfo depth{
			.sType			  = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable  = def.depth_test ? VK_TRUE : VK_FALSE,
			.depthWriteEnable = def.depth_write ? VK_TRUE : VK_FALSE,
			.depthCompareOp	  = vk::to_vk_compare(def.depth_compare),
		};

		VkPipelineColorBlendAttachmentState blend_attachments[MAX_COLOR_ATTACHMENTS];
		VkFormat color_formats[MAX_COLOR_ATTACHMENTS];

		for (u32 i = 0; i < def.color_count; ++i)
		{
			blend_attachments[i] = to_vk_blend(def.blend);
			color_formats[i]	 = vk::format_info(def.color_formats[i]).vk;
		}

		const VkPipelineColorBlendStateCreateInfo blend{
			.sType			 = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = def.color_count,
			.pAttachments	 = blend_attachments,
		};

		const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

		const VkPipelineDynamicStateCreateInfo dynamic{
			.sType			   = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = static_cast<u32>(std::size(dynamic_states)),
			.pDynamicStates	   = dynamic_states,
		};

		// Dynamic rendering: attachment formats ride the pNext chain, render pass stays null.
		const VkPipelineRenderingCreateInfo rendering{
			.sType					 = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount	 = def.color_count,
			.pColorAttachmentFormats = color_formats,
			.depthAttachmentFormat	 = vk::format_info(def.depth_format).vk,
		};

		const VkGraphicsPipelineCreateInfo info{
			.sType				 = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext				 = &rendering,
			.stageCount			 = static_cast<u32>(std::size(stages)),
			.pStages			 = stages,
			.pVertexInputState	 = &vertex_input,
			.pInputAssemblyState = &input_assembly,
			.pViewportState		 = &viewport,
			.pRasterizationState = &raster,
			.pMultisampleState	 = &multisample,
			.pDepthStencilState	 = &depth,
			.pColorBlendState	 = &blend,
			.pDynamicState		 = &dynamic,
			.layout				 = m_backend->descriptor_heap.pipeline_layout(),
		};

		VkPipeline pipeline	  = VK_NULL_HANDLE;
		const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);

		// The pipeline holds the compiled code from here; modules are just SPIR-V envelopes.
		vkDestroyShaderModule(device, vertex, nullptr);
		vkDestroyShaderModule(device, fragment, nullptr);

		if (result != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: pipeline '{}' creation failed: {}", def.name, vk::result_name(result));
			return {};
		}

		const GraphicsPipelineHandle handle = m_backend->resources.graphics_pipelines.insert(
			vk::PipelineData{.pipeline = pipeline, .layout = info.layout});

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: graphics pipeline pool exhausted ({})", def.name);
			vkDestroyPipeline(device, pipeline, nullptr);
			return {};
		}

		vk::set_name(m_backend->context, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(pipeline), def.name);

		return handle;
	}

	ComputePipelineHandle Device::create_compute_pipeline(const ComputePipelineDef& def) noexcept
	{
		EMBER_GPU_GUARD({});

		if (!::ember::gpu::is_valid(def))
		{
			EMBER_ERROR("gpu: compute pipeline '{}' has an invalid def", def.name);
			return {};
		}

		const VkDevice device		= m_backend->context.device;
		const VkShaderModule shader = create_module(device, def.shader.code);

		if (shader == VK_NULL_HANDLE)
		{
			EMBER_ERROR("gpu: compute pipeline '{}' shader module creation failed", def.name);
			return {};
		}

		const VkComputePipelineCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage =
				{
					.sType	= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
					.stage	= VK_SHADER_STAGE_COMPUTE_BIT,
					.module = shader,
					.pName	= def.shader.entry,
				},
			.layout = m_backend->descriptor_heap.pipeline_layout(),
		};

		VkPipeline pipeline	  = VK_NULL_HANDLE;
		const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);

		vkDestroyShaderModule(device, shader, nullptr);

		if (result != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: compute pipeline '{}' creation failed: {}", def.name, vk::result_name(result));
			return {};
		}

		const ComputePipelineHandle handle = m_backend->resources.compute_pipelines.insert(
			vk::PipelineData{.pipeline = pipeline, .layout = info.layout});

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: compute pipeline pool exhausted ({})", def.name);
			vkDestroyPipeline(device, pipeline, nullptr);
			return {};
		}

		vk::set_name(m_backend->context, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(pipeline), def.name);

		return handle;
	}

	void Device::destroy(ComputePipelineHandle handle) noexcept
	{
		EMBER_GPU_GUARD();

		vk::PipelineData* data = m_backend->resources.compute_pipelines.get(handle);
		if (data == nullptr)
			return;

		m_backend->destroy_queue.destroy(data->pipeline);
		(void)m_backend->resources.compute_pipelines.erase(handle);
	}

	bool Device::is_valid(ComputePipelineHandle handle) const noexcept
	{
		return m_backend != nullptr && m_backend->resources.compute_pipelines.contains(handle);
	}

	void Device::destroy(GraphicsPipelineHandle handle) noexcept
	{
		EMBER_GPU_GUARD();

		vk::PipelineData* data = m_backend->resources.graphics_pipelines.get(handle);
		if (data == nullptr)
			return;

		m_backend->destroy_queue.destroy(data->pipeline);
		(void)m_backend->resources.graphics_pipelines.erase(handle);
	}

	bool Device::is_valid(GraphicsPipelineHandle handle) const noexcept
	{
		return m_backend != nullptr && m_backend->resources.graphics_pipelines.contains(handle);
	}
}
