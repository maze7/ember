#include "SDL3/SDL_gpu.h"
#include "SDL3_shadercross/SDL_shadercross.h"
#include "platform/window.h"
#include "graphics/sdl/render_device_sdl.h"
#include "graphics/draw_cmd.h"
#include "graphics/target.h"

#include <span>

using namespace Ember;

namespace
{
    SDL_GPUTextureFormat to_sdl_gpu_texture_format(TextureFormat format) {
        switch (format) {
			case TextureFormat::R8G8B8A8:
				return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			case TextureFormat::R8:
				return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
			case TextureFormat::Depth24Stencil8:
				return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
			case TextureFormat::Color:
				return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			default:
				throw Exception("Unknown texture format");
        }
    }

	SDL_GPUSamplerAddressMode to_sdl_wrap_mode(TextureWrap wrap) {
        switch (wrap) {
			case TextureWrap::Repeat:
				return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
			case TextureWrap::MirroredRepeat:
				return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
			case TextureWrap::Clamp:
				return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			default:
				throw Exception("Unkown texture wrap mode");
		}
	}

	SDL_GPUBufferUsageFlags to_sdl_buffer_usage(BufferUsage usage) {
		switch (usage) {
			case BufferUsage::Vertex:
				return SDL_GPU_BUFFERUSAGE_VERTEX;
			case BufferUsage::Index:
				return SDL_GPU_BUFFERUSAGE_INDEX;
			default:
				throw Exception("Unknown buffer usage");
		}
	}

	SDL_GPUFilter to_sdl_filter(TextureFilter filter) {
		switch (filter) {
			case TextureFilter::Nearest:
				return SDL_GPU_FILTER_NEAREST;
			case TextureFilter::Linear:
				return SDL_GPU_FILTER_LINEAR;
		};
	}

	bool is_depth_texture_format(SDL_GPUTextureFormat format) {
		switch (format) {
			case SDL_GPU_TEXTUREFORMAT_D16_UNORM:
			case SDL_GPU_TEXTUREFORMAT_D24_UNORM:
			case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
			case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
			case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT:
				return true;
			default:
				return false;
		}
	}

	SDL_GPUColorTargetBlendState get_blend_state(BlendMode blend) {
		constexpr auto get_factor = [](BlendFactor factor) {
			using enum BlendFactor;
			switch (factor) {
				case Zero: 					return SDL_GPU_BLENDFACTOR_ZERO;
				case One:					return SDL_GPU_BLENDFACTOR_ONE;
				case SrcColor:				return SDL_GPU_BLENDFACTOR_SRC_COLOR;
				case OneMinusSrcColor: 		return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
				case DstColor: 				return SDL_GPU_BLENDFACTOR_DST_COLOR;
				case OneMinusDstColor: 		return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
				case SrcAlpha:				return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
				case OneMinusSrcAlpha: 		return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
				case DstAlpha:				return SDL_GPU_BLENDFACTOR_DST_ALPHA;
				case OneMinusDstAlpha:		return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
				case ConstantColor:			return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
				case OneMinusConstantColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
				case SrcAlphaSaturate: 		return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
			}
		};

		constexpr auto get_op = [](BlendOp op) {
			using enum BlendOp;
			switch (op) {
				case Add: 				return SDL_GPU_BLENDOP_ADD;
				case Subtract:			return SDL_GPU_BLENDOP_SUBTRACT;
				case ReverseSubtract: 	return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
				case Min: 				return SDL_GPU_BLENDOP_MIN;
				case Max:				return SDL_GPU_BLENDOP_MAX;
			}
		};

		constexpr auto get_flags = [](BlendMask mask) {
			SDL_GPUColorComponentFlags flags{};
			if (bitmask_has(mask, BlendMask::Red)) flags |= SDL_GPU_COLORCOMPONENT_R;
			if (bitmask_has(mask, BlendMask::Green)) flags |= SDL_GPU_COLORCOMPONENT_G;
			if (bitmask_has(mask, BlendMask::Blue)) flags |= SDL_GPU_COLORCOMPONENT_B;
			if (bitmask_has(mask, BlendMask::Alpha)) flags |= SDL_GPU_COLORCOMPONENT_A;
			return flags;
		};

		SDL_GPUColorTargetBlendState state = {
			.src_color_blendfactor = get_factor(blend.color_src),
			.dst_color_blendfactor = get_factor(blend.color_dst),
			.color_blend_op = get_op(blend.color_op),
			.src_alpha_blendfactor = get_factor(blend.alpha_src),
			.dst_alpha_blendfactor = get_factor(blend.alpha_dst),
			.alpha_blend_op = get_op(blend.alpha_op),
			.color_write_mask = get_flags(blend.mask),
			.enable_blend = true,
		};

		return state;
	}

	SDL_GPUVertexElementFormat get_vertex_format(VertexType type, bool normalized) {
		switch (type) {
            case VertexType::Float:
            	return SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
            case VertexType::Float2:
            	return SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            case VertexType::Float3:
            	return SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            case VertexType::Float4:
            	return SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
            case VertexType::Byte4:
                return normalized ? SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM
                                  : SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_BYTE4;
            case VertexType::UByte4:
                return normalized ? SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM
                                  : SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4;
            case VertexType::Short2:
                return normalized ? SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM
                                  : SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_SHORT2;
            case VertexType::UShort2:
                return normalized ? SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM
                                  : SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_USHORT2;
            case VertexType::Short4:
                return normalized ? SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM
                                  : SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_SHORT4;
            case VertexType::UShort4:
                return normalized ? SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM
                                  : SDL_GPUVertexElementFormat::SDL_GPU_VERTEXELEMENTFORMAT_USHORT4;
            default:
            	throw std::invalid_argument("Invalid Vertex Format");
        }
	}
}

RenderDeviceSDL::RenderDeviceSDL(Window& window) : m_window(window) {
#ifdef __APPLE__
    auto shader_format = SDL_GPU_SHADERFORMAT_MSL;
#else
    auto shader_format = SDL_GPU_SHADERFORMAT_SPIRV;
#endif // __APPLE__
    EMBER_INFO("Initializing renderer...");

    m_gpu = SDL_CreateGPUDevice(shader_format, true, nullptr);
	if (m_gpu == nullptr)
		throw Exception(fmt::format("Error creating GPU Device: {}", SDL_GetError()));

    SDL_ClaimWindowForGPUDevice(m_gpu, m_window.native_handle());

    // set present mode depending on what is available
    if (SDL_WindowSupportsGPUPresentMode(m_gpu, m_window.native_handle(), SDL_GPU_PRESENTMODE_IMMEDIATE)) {
        SDL_SetGPUSwapchainParameters(m_gpu, m_window.native_handle(), SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_IMMEDIATE);
    }

    // init command buffers
    reset_command_buffers();

    // create staging buffers (textures & buffer)
    SDL_GPUTransferBufferCreateInfo info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = STAGING_BUFFER_SIZE, // 16mb
        .props = 0,
    };

    m_texture_staging = SDL_CreateGPUTransferBuffer(m_gpu, &info);
    m_buffer_staging = SDL_CreateGPUTransferBuffer(m_gpu, &info);

    // Create default texture
    u8 white_pixel[4] = { 255, 255, 255, 255 };
    m_default_texture = create_texture({
        .size = { 1, 1 },
        .format = TextureFormat::R8G8B8A8,
        .data = std::span(white_pixel),
    });

    // Create framebuffer (default render target)
    m_framebuffer = make_ref<Target>(this, window.size());

    // Default to 3 frames in flight
    SDL_SetGPUAllowedFramesInFlight(m_gpu, 3);
}

RenderDeviceSDL::~RenderDeviceSDL() {
    EMBER_INFO("Disposing renderer...");

    flush_commands(false);
    SDL_ReleaseGPUTransferBuffer(m_gpu, m_texture_staging);
    SDL_ReleaseGPUTransferBuffer(m_gpu, m_buffer_staging);
    wait_idle();

    m_framebuffer.reset();

    // Cleanup allocated shaders
    for (auto shader : m_shaders) {
        SDL_ReleaseGPUShader(m_gpu, shader.vertex);
        SDL_ReleaseGPUShader(m_gpu, shader.fragment);
    }

    // Cleanup allocated textures (including RenderTarget attachments)
    for (auto tex : m_textures) {
        SDL_ReleaseGPUTexture(m_gpu, tex.texture);
    }

    // Cleanup allocated buffers
    for (auto buf : m_buffers) {
    	SDL_ReleaseGPUBuffer(m_gpu, buf.buffer);
    }

    // Final cleanup
    SDL_ReleaseWindowFromGPUDevice(m_gpu, m_window.native_handle());
    SDL_DestroyGPUDevice(m_gpu);
}

void RenderDeviceSDL::clear(ClearInfo clear_info, Ref<Target> target) {
    if (clear_info.mask != ClearMask::None) {
        begin_render_pass({
            .color = (i32) clear_info.mask & (i32) ClearMask::Color ? std::optional<Color>(clear_info.color) : std::nullopt,
            .depth = (i32) clear_info.mask & (i32) ClearMask::Depth ? std::optional<float>(clear_info.depth) : std::nullopt,
            .stencil = (i32) clear_info.mask & (i32) ClearMask::Stencil
                        ? std::optional<int>(clear_info.stencil)
                        : std::nullopt,
        }, target.get());
    }
}

void RenderDeviceSDL::wait_idle() {
    SDL_WaitForGPUIdle(m_gpu);
}

void RenderDeviceSDL::submit(const DrawCommand& cmd) {
	auto& mat = cmd.material;
	auto& shader = mat.shader;
	auto& target = cmd.target;

	// Begin render pass
	if (!begin_render_pass(ClearInfo(), target))
		return;

	// Set viewport for the render pass
	auto pass_target_size = m_render_pass_target->size();
	auto next_viewport = cmd.viewport.value_or(Recti(0, 0, pass_target_size.x, pass_target_size.y));
	if (m_render_pass_viewport != next_viewport) {
		m_render_pass_viewport = next_viewport;
		SDL_GPUViewport viewport = {
			.x = (float) next_viewport.x,
			.y = (float) next_viewport.y,
			.w = (float) next_viewport.w,
			.h = (float) next_viewport.h,
			.min_depth = 0,
			.max_depth = 1,
		};

		SDL_SetGPUViewport(m_render_pass, &viewport);
	}

	// Set scissor for the render pass, defaults to viewport if none is provided.
	auto next_scissor = cmd.scissor.value_or(next_viewport);
	if (m_render_pass_scissor != next_scissor) {
		m_render_pass_scissor = next_scissor;
		SDL_Rect rect = {
			.x = next_scissor.x,
			.y = next_scissor.y,
			.w = next_scissor.w,
			.h = next_scissor.h,
		};

		SDL_SetGPUScissor(m_render_pass, &rect);
	}

	// Retrieve cached PSO (or create on-demand)
	auto pso = get_pipeline(cmd);
	if (pso != m_render_pass_pso) {
		SDL_BindGPUGraphicsPipeline(m_render_pass, pso);
		m_render_pass_pso = pso;
	}

	// Bind index buffer if it has changed or been updated
	if (!cmd.index_buffer.is_null()) {
		auto ib_data = m_buffers.get(cmd.index_buffer);
		if (m_render_pass_index_buffer != cmd.index_buffer || ib_data->dirty) {
			m_render_pass_index_buffer = cmd.index_buffer;
			ib_data->dirty = false;

            SDL_GPUBufferBinding index_binding = { .buffer = ib_data->buffer, .offset = 0 };
            // Select index element size based on DrawCommand::index_size (2 or 4 bytes)
            SDL_GPUIndexElementSize index_element_size =
                (cmd.index_size == sizeof(u16))
                    ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                    : SDL_GPU_INDEXELEMENTSIZE_32BIT;

            SDL_BindGPUIndexBuffer(m_render_pass, &index_binding, index_element_size);
		}
	} else {
		m_render_pass_index_buffer = {}; // Reset if no index buffer
	}

	// Check if vertex buffers need to be rebound
	bool rebind_vertex_buffers = m_render_pass_vertex_buffers.size() != cmd.vertex_buffers.size();
	if (!rebind_vertex_buffers) {
		for (size_t i = 0; i < cmd.vertex_buffers.size(); ++i) {
			if (m_render_pass_vertex_buffers[i] != cmd.vertex_buffers[i].buffer.handle || m_buffers.get(cmd.vertex_buffers[i].buffer.handle)->dirty) {
				rebind_vertex_buffers = true;
				break;
			}
		}
	}

	// Bind vertex buffers
	if (rebind_vertex_buffers) {
		m_render_pass_vertex_buffers.clear();
		std::vector<SDL_GPUBufferBinding> vertex_bindings;
		vertex_bindings.reserve(cmd.vertex_buffers.size());

		for (const auto& vb_info : cmd.vertex_buffers) {
			if (!vb_info.buffer.handle.is_null()) {
				auto vb_data = m_buffers.get(vb_info.buffer.handle);
				vb_data->dirty = false;
				vertex_bindings.push_back({ .buffer = vb_data->buffer, .offset = 0 });
				m_render_pass_vertex_buffers.push_back(vb_info.buffer.handle);
			}
		}

		if (!vertex_bindings.empty()) {
			SDL_BindGPUVertexBuffers(m_render_pass, 0, vertex_bindings.data(), vertex_bindings.size());
		}
	}

	// Bind fragment samplers
	if (!mat.fragment.samplers.empty()) {
		std::vector<SDL_GPUTextureSamplerBinding> sampler_bindings;
		sampler_bindings.reserve(mat.fragment.samplers.size());

		for (const auto& mat_sampler : mat.fragment.samplers) {
			SDL_GPUTexture* texture_to_sample;
			if (!mat_sampler.texture.is_null()) {
				auto tex_data = m_textures.get(mat_sampler.texture);

				// Fallback to default texture if texture doesn't exist
				if (tex_data == nullptr) {
					tex_data = m_textures.get(m_default_texture);
				}

				// If texture is multisampled, sample from its resolve texture instead
				texture_to_sample = !tex_data->msaa_resolve_texture.is_null()
					? m_textures.get(tex_data->msaa_resolve_texture)->texture
					: tex_data->texture;
			} else {
				texture_to_sample = m_textures.get(m_default_texture)->texture;
			}

			sampler_bindings.push_back({
				.texture = texture_to_sample,
				.sampler = get_sampler(mat_sampler.sampler)
			});
		}

		SDL_BindGPUFragmentSamplers(m_render_pass, 0, sampler_bindings.data(), sampler_bindings.size());
	}

	// Bind vertex samplers
	if (!mat.vertex.samplers.empty()) {
		std::vector<SDL_GPUTextureSamplerBinding> sampler_bindings;
		sampler_bindings.reserve(mat.vertex.samplers.size());

		for (const auto& mat_sampler : mat.vertex.samplers) {
			SDL_GPUTexture* texture_to_sample;
			if (!mat_sampler.texture.is_null()) {
				auto tex_data = m_textures.get(mat_sampler.texture);
				texture_to_sample = !tex_data->msaa_resolve_texture.is_null()
					? m_textures.get(tex_data->msaa_resolve_texture)->texture
					: tex_data->texture;
			} else {
				texture_to_sample = m_textures.get(m_default_texture)->texture;
			}

			sampler_bindings.push_back({
				.texture = texture_to_sample,
				.sampler = get_sampler(mat_sampler.sampler)
			});
		}

		SDL_BindGPUVertexSamplers(m_render_pass, 0, sampler_bindings.data(), sampler_bindings.size());
	}

	// Push fragment uniforms
	for (uint32_t i = 0; i < Material::Stage::MAX_UNIFORM_BUFFERS; ++i) {
		auto uniform_buffer = mat.fragment.get_uniform_buffer(i);
		if (!uniform_buffer.empty()) {
			SDL_PushGPUFragmentUniformData(m_cmd_render, i, (void*)uniform_buffer.data(), uniform_buffer.size_bytes());
		}
	}

	// Push vertex uniforms
	for (uint32_t i = 0; i < Material::Stage::MAX_UNIFORM_BUFFERS; ++i) {
		auto uniform_buffer = mat.vertex.get_uniform_buffer(i);
		if (!uniform_buffer.empty()) {
			SDL_PushGPUVertexUniformData(m_cmd_render, i, (void*)uniform_buffer.data(), uniform_buffer.size_bytes());
		}
	}

	// Perform draw call
	if (!cmd.index_buffer.is_null()) {
		SDL_DrawGPUIndexedPrimitives(
			m_render_pass,
			cmd.index_count,
			std::max(1u, cmd.instance_count),
			cmd.index_offset,
			cmd.vertex_offset,
			0 // first_instance
		);
	} else {
		SDL_DrawGPUPrimitives(
			m_render_pass,
			cmd.vertex_count,
			std::max(1u, cmd.instance_count),
			cmd.vertex_offset,
			0 // first_instance
		);
	}
}

void RenderDeviceSDL::present() {
    end_copy_pass();
    end_render_pass();

    // Wait for fences for the current frame to complete
    if (m_fences[m_frame][0] || m_fences[m_frame][1]) {
  		SDL_WaitForGPUFences(m_gpu, true, m_fences[m_frame], 2);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][0]);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][1]);
    }

    // If swapchain can be acquired, blit framebuffer to it
    SDL_GPUTexture* swapchain_texture = nullptr;
    glm::uvec2 swapchain_size;

    if (SDL_AcquireGPUSwapchainTexture(m_cmd_render, m_window.native_handle(), &swapchain_texture, &swapchain_size.x, &swapchain_size.y)) {
        // SDL_AcquireGPUSwapchainTexture can return true, but no texture for a variety of reasons
		// - window is minimized
		// - awaiting previous frame render
		if (swapchain_texture)
		{
			auto color_attachment = m_textures.get(m_framebuffer->attachments()[0]);
			EMBER_ASSERT(m_framebuffer != nullptr && color_attachment != nullptr);

			u32 blit_width = min(color_attachment->size.x, swapchain_size.x);
			u32 blit_height = min(color_attachment->size.y, swapchain_size.y);

			SDL_GPUBlitInfo blit_info;
			blit_info.source.texture = color_attachment->texture;
			blit_info.source.mip_level = 0;
			blit_info.source.layer_or_depth_plane = 0;
			blit_info.source.x = 0;
			blit_info.source.y = 0;
			blit_info.source.w = blit_width;
			blit_info.source.h = blit_height;

			blit_info.destination.texture = swapchain_texture;
			blit_info.destination.mip_level = 0;
			blit_info.destination.layer_or_depth_plane = 0;
			blit_info.destination.x = 0;
			blit_info.destination.y = 0;
			blit_info.destination.w = blit_width;
			blit_info.destination.h = blit_height;

			blit_info.load_op = SDL_GPU_LOADOP_DONT_CARE;
			blit_info.clear_color.r = 0;
			blit_info.clear_color.g = 0;
			blit_info.clear_color.b = 0;
			blit_info.clear_color.a = 0;
			blit_info.flip_mode = SDL_FLIP_NONE;
			blit_info.filter = SDL_GPU_FILTER_LINEAR;
			blit_info.cycle = false;

			SDL_BlitGPUTexture(m_cmd_render, &blit_info);

			// Growth condition: if the current buffer is smaller than the swapchain.
			// A slightly larger buffer is created to avoid frequent re-allocations.
			if (color_attachment->size.x < swapchain_size.x || color_attachment->size.y < swapchain_size.y) {
				glm::uvec2 new_size = { swapchain_size.x + 64, swapchain_size.y + 64 };
				m_framebuffer = make_ref<Target>(new_size);
				Log::trace("Framebuffer grown to: {}x{}", new_size.x, new_size.y);
			} else if (color_attachment->size.x > swapchain_size.x + 128 || color_attachment->size.y > swapchain_size.y + 128) {
				m_framebuffer = make_ref<Target>(swapchain_size);
				Log::trace("Framebuffer shrunk to: {}x{}", swapchain_size.x, swapchain_size.y);
			}
		}
    }

    flush_commands_and_acquire_fences();
    m_frame = (m_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void RenderDeviceSDL::flush_commands_and_acquire_fences() {
	end_copy_pass();
	end_render_pass();

	m_fences[m_frame][0] = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd_transfer);
	m_fences[m_frame][1] = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd_render);

	if (!m_fences[m_frame][0]) {
		Log::warn("Unable to acquire upload fence: {}", SDL_GetError());
	} else if (!m_fences[m_frame][1]) {
		Log::warn("Unable to acquire render fence: {}", SDL_GetError());
	}

	m_cmd_transfer = nullptr;
	m_cmd_render = nullptr;
	reset_command_buffers();
}

void RenderDeviceSDL::flush_commands_and_stall() {
	flush_commands_and_acquire_fences();

	if (m_fences[m_frame][0] || m_fences[m_frame][1]) {
		SDL_WaitForGPUFences(m_gpu, true, m_fences[m_frame], 2);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][0]);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][1]);
	}
}

Handle<Texture> RenderDeviceSDL::create_texture(const TextureDef& def) {
    auto sdl_format =  to_sdl_gpu_texture_format(def.format);

    const auto sdl_sample_count = [](SampleCount count) {
    	switch (count) {
     		case SampleCount::One: return SDL_GPU_SAMPLECOUNT_1;
       		case SampleCount::Two: return SDL_GPU_SAMPLECOUNT_2;
         	case SampleCount::Four: return SDL_GPU_SAMPLECOUNT_4;
          	case SampleCount::Eight: return SDL_GPU_SAMPLECOUNT_8;
     	}
    };

    SDL_GPUTextureCreateInfo info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = sdl_format,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width =  def.size.x,
        .height = def.size.y,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = sdl_sample_count(def.sample_count)
    };

    if (def.is_target_attachment) {
        if (def.format == TextureFormat::Depth24Stencil8)
            info.usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        else
            info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }

    // create texture resource and add to texture pool
    auto texture = SDL_CreateGPUTexture(m_gpu, &info);

    if (!texture) {
        Log::error("Failed to create texture: {}", SDL_GetError());
    }

    // Add the texture data to the pool
    auto handle = m_textures.emplace(texture, sdl_format, def.size, def.is_target_attachment);

    // If multisampled, create a resolve texture and link it
	if (def.sample_count != SampleCount::One && def.is_target_attachment) {
		TextureDef resolve_def = def;
		resolve_def.sample_count = SampleCount::One; // Resolve texture is always 1x sampled

		// Ensure the resolve texture can be sampled
		resolve_def.is_target_attachment = false;

		auto resolve_handle = create_texture(resolve_def);

		// Link the resolve texture to the main multisampled texture
		m_textures.get(handle)->msaa_resolve_texture = resolve_handle;
	}

    // Upload initial data if provided
    if (def.data.size() > 0) {
        set_texture_data(handle, def.data);
    }

    return handle;
}

void RenderDeviceSDL::set_texture_data(Handle<Texture> handle, std::span<u8> data) {
    EMBER_ASSERT(!handle.is_null());

    // Utility to round to nearest memory alignment
    static constexpr auto round_alignment = [](u32 value, u32 alignment) {
        return alignment * ((value + alignment - 1) / alignment);
    };

    // Retrieve the hot/cold texture data from the Texture pool.
    auto tex = m_textures.get(handle);
    EMBER_ASSERT(tex != nullptr);

    // Align buffer offset to texture format requirements
    m_texture_staging_offset = round_alignment(
        m_texture_staging_offset,
        SDL_GPUTextureFormatTexelBlockSize(tex->format)
    );

    // Initialize upload parameters
    SDL_GPUTransferBuffer* staging_buffer = m_texture_staging;
    u32 staging_offset = m_texture_staging_offset;
    bool cycle = (m_texture_staging_offset == 0);
    bool use_temp_buffer = false;

    // Determine buffer strategy based on data size
    const u32 data_size = static_cast<u32>(data.size());

    // acquire staging buffer
    if (data_size >= STAGING_BUFFER_SIZE) {
        // Data is too large for main buffer - create temporary buffer
        SDL_GPUTransferBufferCreateInfo temp_info = {
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
            .size = data_size,
            .props = 0
        };

        staging_buffer = SDL_CreateGPUTransferBuffer(m_gpu, &temp_info);
        use_temp_buffer = true;
        staging_offset = 0;
        cycle = false;
    } else if (m_texture_staging_offset + data_size >= STAGING_BUFFER_SIZE) {
        // Data doesn't fit in remaining buffer space
        if (m_texture_staging_cycle < MAX_STAGING_CYCLE_COUNT) {
            // We can cycle without stalling
            cycle = true;
            m_texture_staging_cycle++;
            m_texture_staging_offset = 0;
            staging_offset = 0;
        }
    }

    // Map buffer and copy data
    u8* mapped_memory = static_cast<u8*>(SDL_MapGPUTransferBuffer(m_gpu, staging_buffer, cycle)) + staging_offset;
    std::memcpy(mapped_memory, data.data(), data_size);
    SDL_UnmapGPUTransferBuffer(m_gpu, staging_buffer);

    // Prepare upload to GPU
    begin_copy_pass();

    SDL_GPUTextureTransferInfo transfer_info = {
        .transfer_buffer = staging_buffer,
        .offset = staging_offset,
        .pixels_per_row = tex->size.x,
        .rows_per_layer = tex->size.y,
    };

    SDL_GPUTextureRegion target_region = {
        .texture = tex->texture,
        .mip_level = 0,
        .layer = 0,
        .x = 0, .y = 0, .z = 0,
        .w = tex->size.x,
        .h = tex->size.y,
        .d = 1
    };

    SDL_UploadToGPUTexture(m_copy_pass, &transfer_info, &target_region, cycle);

    // Clean up based on buffer type
    if (use_temp_buffer) {
        SDL_ReleaseGPUTransferBuffer(m_gpu, staging_buffer);
    } else {
        m_texture_staging_offset += data_size;
    }
}

void RenderDeviceSDL::dispose_texture(Handle<Texture> handle) {
    if (auto data = m_textures.get(handle)) {
        Log::trace("Destroying texture: [slot: {}, gen: {}]", handle.index, handle.generation);
        SDL_ReleaseGPUTexture(m_gpu, data->texture);
        m_textures.erase(handle);
    }
}

Handle<Shader> RenderDeviceSDL::create_shader(const ShaderDef& def) {

    // Vertex shader info
    SDL_ShaderCross_SPIRV_Info vertex_create_info = {
        .bytecode = def.vertex.code.data(),
        .bytecode_size = def.vertex.code.size(),
        .entrypoint = def.vertex.entrypoint,
        .shader_stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX,
        .name = def.vertex.entrypoint,
    };

    SDL_ShaderCross_GraphicsShaderMetadata vertex_meta = {
        .num_samplers = def.vertex.num_samplers,
        .num_uniform_buffers = def.vertex.num_uniform_buffers,
    };

    // Fragment shader info
    SDL_ShaderCross_SPIRV_Info fragment_create_info = {
        .bytecode = def.fragment.code.data(),
        .bytecode_size = def.fragment.code.size(),
        .entrypoint = def.fragment.entrypoint,
        .shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT,
        .name = def.fragment.entrypoint
    };

    SDL_ShaderCross_GraphicsShaderMetadata fragment_meta = {
        .num_samplers = def.fragment.num_samplers,
        .num_uniform_buffers = def.fragment.num_uniform_buffers
    };

    // Compile shaders
    auto vertex = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(m_gpu, &vertex_create_info, &vertex_meta);
    auto fragment = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(m_gpu, &fragment_create_info, &fragment_meta);
    if (!vertex || !fragment) {
        if (vertex) SDL_ReleaseGPUShader(m_gpu, vertex);
        if (fragment) SDL_ReleaseGPUShader(m_gpu, fragment);
        throw Exception(fmt::format("Failed to compile shader: {}", SDL_GetError()));
    }

    // Add to shader pool and return Handle
    return m_shaders.emplace(vertex, fragment);
}

void RenderDeviceSDL::dispose_shader(Handle<Shader> handle) {
	if (auto shader = m_shaders.get(handle)) {
		// Dispose all PSOs that use this shader
		for (u64 pso_hash : shader->pso_hashes) {
			if (auto it = m_psos.find(pso_hash); it != m_psos.end()) {
				SDL_ReleaseGPUGraphicsPipeline(m_gpu, it->second);
				m_psos.erase(it);
			}
		}

		// Dispose Shader
		SDL_ReleaseGPUShader(m_gpu, shader->vertex);
		SDL_ReleaseGPUShader(m_gpu, shader->fragment);
		m_shaders.erase(handle);
	}
}

Handle<Buffer> RenderDeviceSDL::create_buffer(const BufferDef& def) {
	// Add a check for zero-sized buffers if they are not intended
	if (def.size == 0 && def.data.empty()) {
		// Log a warning or handle as an error, depending on your engine's design
		Log::warn("Creating a zero-sized GPU buffer.");
		return Handle<Buffer>::null;
	}

	auto sdl_usage = to_sdl_buffer_usage(def.usage);

	SDL_GPUBufferCreateInfo info = {
		.usage = sdl_usage,
		.size = def.size,
		.props = 0,
	};

	auto buffer = SDL_CreateGPUBuffer(m_gpu, &info);
	if (!buffer) {
		Log::error("Failed to create buffer: {}", SDL_GetError());
	}

	auto handle = m_buffers.emplace(buffer, sdl_usage, def.size, false);

	if (!def.data.empty()) {
		set_buffer_data(handle, def.data, 0);
	}

	return handle;
}

void RenderDeviceSDL::set_buffer_data(Handle<Buffer> handle, std::span<const std::byte> data, u32 offset) {
	if (auto buf = m_buffers.get(handle)) {
		EMBER_ASSERT(buf != nullptr);

		const u32 data_size = static_cast<u32>(data.size());

		// Check if we need to resize the buffer
		u32 required_size = data_size + offset;
		if (required_size > buf->size) {
			Log::trace("Resizing GPU buffer [slot: {}, gen: {}] from {} to {} bytes", handle.index, handle.generation, buf->size, required_size);

			SDL_ReleaseGPUBuffer(m_gpu, buf->buffer);
			SDL_GPUBufferCreateInfo info = {
				.usage = buf->usage,
				.size = required_size,
			};

			buf->buffer = SDL_CreateGPUBuffer(m_gpu, &info);
			buf->size = required_size;
		}

		// Initialize upload parameters
		SDL_GPUTransferBuffer* staging_buffer = m_buffer_staging;
		u32 staging_offset = m_buffer_staging_offset;
		bool cycle = (m_buffer_staging_offset == 0);
		bool use_temp_buffer = false;

		// Determine buffer strategy based on data size
		if (data_size >= STAGING_BUFFER_SIZE) {
			// Data is too large, create a temporary buffer
			SDL_GPUTransferBufferCreateInfo temp_info = {
				.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
				.size = data_size
			};

			use_temp_buffer = true;
			staging_offset = 0;
			cycle = false;
		} else if (m_buffer_staging_offset + data_size >= STAGING_BUFFER_SIZE) {
			// Not enough space, cycle the main staging buffer
			if (m_buffer_staging_cycle < MAX_STAGING_CYCLE_COUNT) {
				cycle = true;
				m_buffer_staging_cycle++;
				m_buffer_staging_offset = 0;
				staging_offset = 0;
			} else {
				// We've cycled too many times, stall the GPU to be safe
				flush_commands_and_stall();
				cycle = true;
				staging_offset = 0;
			}
		}

		// Map buffer and copy data
		std::byte* mapped_memory = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(m_gpu, staging_buffer, cycle)) + staging_offset;
		std::memcpy(mapped_memory, data.data(), data_size);
		SDL_UnmapGPUTransferBuffer(m_gpu, staging_buffer);

		// Record the upload command
		begin_copy_pass();

		SDL_GPUTransferBufferLocation src = {
			.transfer_buffer = staging_buffer,
			.offset = staging_offset
		};

		SDL_GPUBufferRegion dst = {
			.buffer = buf->buffer,
			.offset = offset,
			.size = data_size
		};

		SDL_UploadToGPUBuffer(m_copy_pass, &src, &dst, cycle);

		buf->dirty = true; // Mark as dirty for render pass binding logic

		// Clean up
		if (use_temp_buffer) {
			SDL_ReleaseGPUTransferBuffer(m_gpu, staging_buffer);
		} else {
			m_buffer_staging_offset += data_size;
		}
	}
}

void RenderDeviceSDL::dispose_buffer(Handle<Buffer> handle) {
	if (auto data = m_buffers.get(handle)) {
		Log::trace("Destroying buffer: [slot: {}, gen: {}]", handle.index, handle.generation);
		SDL_ReleaseGPUBuffer(m_gpu, data->buffer);
		m_buffers.erase(handle);
	}
}

SDL_GPUGraphicsPipeline* RenderDeviceSDL::get_pipeline(const DrawCommand& cmd) {

	auto hash = combined_hash(
		cmd.material.shader,
		cmd.cull_mode,
		cmd.depth_compare,
		cmd.depth_test_enabled,
		cmd.depth_write_enabled,
		cmd.blend_mode
	);

	if (!cmd.index_buffer.is_null())
		hash = combined_hash(hash, cmd.index_size);

	// Hash the vertex buffer formats & instance input rate
	for (const auto& [buffer, instance_input_rate] : cmd.vertex_buffers)
		hash = combined_hash(hash, buffer.format, instance_input_rate);

	// Include the target in the hash
	auto target_formats = get_target_formats_and_sample_count(cmd.target);
    for (const auto& info : target_formats) {
        hash = combined_hash(hash, info.format, info.sample_count);
    }

	// Look for an existing PSO
	if (auto it = m_psos.find(hash); it != m_psos.end())
		return it->second; // Cache hit!

	// Cache miss: create a new PSO
	Log::trace("Creating new PSO for hash: {}", hash);

	// Count the number of vertex attributes
	int vertex_attribute_count = 0;
	for (const auto& vb : cmd.vertex_buffers)
		vertex_attribute_count += vb.buffer.format.elements.size();

	std::vector<SDL_GPUVertexBufferDescription> vertex_bindings(cmd.vertex_buffers.size());
	std::vector<SDL_GPUVertexAttribute> vertex_attributes(vertex_attribute_count);
	std::array<SDL_GPUColorTargetDescription, MAX_COLOR_ATTACHMENTS> color_attachments;

	int color_attachment_count = 0;
	auto depth_stencil_attachment = SDL_GPU_TEXTUREFORMAT_INVALID;
	auto sample_count = SDL_GPU_SAMPLECOUNT_1;
	const auto color_blend_state = get_blend_state(cmd.blend_mode);

	// Configure target attachments
	for (const auto& it : target_formats) {
		if (is_depth_texture_format(it.format)) {
			depth_stencil_attachment = it.format;
		} else if (color_attachment_count < MAX_COLOR_ATTACHMENTS) {
			color_attachments[color_attachment_count++] = {
				.format = it.format,
				.blend_state = color_blend_state
			};
		}

		if (static_cast<int>(it.sample_count) > static_cast<int>(sample_count)) {
			sample_count = it.sample_count;
		}
	}

	// Conmfigure vertex attributes
	int attr_index = 0;
	for (size_t slot = 0; slot < cmd.vertex_buffers.size(); ++slot) {
		const auto& buffer_info = cmd.vertex_buffers[slot];
		int vertex_offset = 0;

		vertex_bindings[slot] = {
			.slot = static_cast<u32>(slot),
			.pitch = static_cast<u32>(buffer_info.buffer.format.stride),
			.input_rate = buffer_info.instance_input_rate
				? SDL_GPU_VERTEXINPUTRATE_INSTANCE
				: SDL_GPU_VERTEXINPUTRATE_VERTEX,
		};

		for (const auto& el : buffer_info.buffer.format.elements) {
			vertex_attributes[attr_index++] = {
				.location = static_cast<u32>(el.index),
				.buffer_slot = static_cast<u32>(slot),
				.format = get_vertex_format(el.type, el.normalized),
				.offset = static_cast<u32>(vertex_offset)
			};
			vertex_offset += vertex_type_size(el.type);
		}
	}

	// Use immediately-invoked lambdas to initialize const variables from switch statements.
	const SDL_GPUCullMode sdl_cull_mode = [&] {
		switch (cmd.cull_mode)
		{
			case CullMode::None:  return SDL_GPU_CULLMODE_NONE;
			case CullMode::Front: return SDL_GPU_CULLMODE_FRONT;
			case CullMode::Back:  return SDL_GPU_CULLMODE_BACK;
			default: throw std::logic_error("Invalid CullMode");
		}
	}();

	const SDL_GPUCompareOp sdl_compare_op = [&] {
		switch (cmd.depth_compare)
		{
			case DepthCompare::Always:          return SDL_GPU_COMPAREOP_ALWAYS;
			case DepthCompare::Never:           return SDL_GPU_COMPAREOP_NEVER;
			case DepthCompare::Less:            return SDL_GPU_COMPAREOP_LESS;
			case DepthCompare::Equal:           return SDL_GPU_COMPAREOP_EQUAL;
			case DepthCompare::LessOrEqual:     return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
			case DepthCompare::Greater:         return SDL_GPU_COMPAREOP_GREATER;
			case DepthCompare::NotEqual:        return SDL_GPU_COMPAREOP_NOT_EQUAL;
			case DepthCompare::GreaterOrEqual:  return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
			default: return SDL_GPU_COMPAREOP_NEVER;
		}
	}();

	auto shader = m_shaders.get(cmd.material.shader);
	EMBER_ASSERT(shader);

	SDL_GPUGraphicsPipelineCreateInfo info = {
		.vertex_shader = shader->vertex,
		.fragment_shader = shader->fragment,
		.vertex_input_state = {
			.vertex_buffer_descriptions = vertex_bindings.data(),
			.num_vertex_buffers = static_cast<u32>(vertex_bindings.size()),
			.vertex_attributes = vertex_attributes.data(),
			.num_vertex_attributes = static_cast<u32>(vertex_attributes.size())
		},
		.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
		.rasterizer_state = {
			.fill_mode = SDL_GPU_FILLMODE_FILL,
			.cull_mode = sdl_cull_mode,
			.front_face = SDL_GPU_FRONTFACE_CLOCKWISE,
		},
		.multisample_state = {
			.sample_count = sample_count,
			.sample_mask = 0, // SDL docs indicate this is unused.
		},
		.depth_stencil_state = {
			.compare_op = sdl_compare_op,
			.compare_mask = 0xFF,
			.write_mask = 0xFF,
			.enable_depth_test = cmd.depth_test_enabled,
			.enable_depth_write = cmd.depth_write_enabled,
			.enable_stencil_test = false, // TODO: Expose stencil state
		},
		.target_info = {
			.color_target_descriptions = color_attachments.data(),
			.num_color_targets = static_cast<u32>(color_attachment_count),
			.depth_stencil_format = depth_stencil_attachment,
			.has_depth_stencil_target = (depth_stencil_attachment != SDL_GPU_TEXTUREFORMAT_INVALID),
		}
	};

	SDL_GPUGraphicsPipeline* pso = SDL_CreateGPUGraphicsPipeline(m_gpu, &info);
	if (!pso) {
		throw Exception(std::string("SDL_CreateGPUGraphicsPipeline failed: {}") + SDL_GetError());
	}

	// Cache the PSO and return it.
	m_psos[hash] = pso;
	shader->pso_hashes.push_back(hash);

	return pso;
}

SDL_GPUSampler* RenderDeviceSDL::get_sampler(TextureSampler sampler) {
	// Check if the sampler already exists in the cache
	if (auto it = m_samplers.find(sampler); it != m_samplers.end())
		return it->second;

	// Convert TextureSampler properties to SDL_GPU types
	SDL_GPUSamplerCreateInfo info = {
		.min_filter = to_sdl_filter(sampler.filter),
		.mag_filter = to_sdl_filter(sampler.filter),
		.address_mode_u = to_sdl_wrap_mode(sampler.wrap_x),
		.address_mode_v = to_sdl_wrap_mode(sampler.wrap_y),
		.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
		.compare_op = SDL_GPU_COMPAREOP_ALWAYS,
		.enable_compare = false
	};

	// Create the sampler
	SDL_GPUSampler* result = SDL_CreateGPUSampler(m_gpu, &info);
	if (!result)
		throw Exception("Failed to create GPU sampler: " + std::string(SDL_GetError()));

	// Cache the newly created sampler, then return.
	m_samplers[sampler] = result;
	return result;
}

bool RenderDeviceSDL::begin_render_pass(ClearInfo clear, Target* target) {
    if (target == nullptr)
        target = m_framebuffer.get();

    // only begin a pass if we're not already in a render pass
    if (m_render_pass && m_render_pass_target == target && !clear.color && !clear.depth && !clear.stencil)
        return true;

    end_render_pass();

    m_render_pass_target = target;

    std::vector<SDL_GPUColorTargetInfo> color_infos;
    color_infos.reserve(target->attachments().size());
    SDL_GPUDepthStencilTargetInfo depth_stencil_info;
    SDL_GPUTexture* depth_stencil_target = nullptr;

    auto clear_color = clear.color.value_or(Color::Transparent);

    // Get color and depth/stencil info from attachments
    for (auto& tex_handle : target->attachments()) {
        auto tex_data = m_textures.get(tex_handle);
        EMBER_ASSERT(tex_data && tex_data->texture);

        if (is_depth_texture_format(tex_data->format)) {
            // This is our depth/stencil attachment
            depth_stencil_target = tex_data->texture;
            depth_stencil_info = {
                .texture = depth_stencil_target,
                .clear_depth = clear.depth.value_or(0),
                .load_op = clear.depth.has_value() ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
                .store_op = SDL_GPU_STOREOP_STORE,
                .stencil_load_op = clear.stencil.has_value() ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
                .stencil_store_op = SDL_GPU_STOREOP_STORE,
                .cycle = clear.depth.has_value() && clear.stencil.has_value(),
                .clear_stencil = static_cast<u8>(clear.stencil.value_or(0)),
            };
        } else {
            // This is a color attachment
            auto& color_info = color_infos.emplace_back();
            color_info.texture = tex_data->texture;
            color_info.clear_color = {
                    .r = static_cast<float>(clear_color.r) / 255.0f,
                    .g = static_cast<float>(clear_color.g) / 255.0f,
                    .b = static_cast<float>(clear_color.b) / 255.0f,
                    .a = static_cast<float>(clear_color.a) / 255.0f,
            };
            color_info.load_op = clear.color.has_value() ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            color_info.cycle = clear.color.has_value();

            // Check for MSAA resolve
            if (!tex_data->msaa_resolve_texture.is_null()) {
                auto resolve_tex_data = m_textures.get(tex_data->msaa_resolve_texture);
                EMBER_ASSERT(resolve_tex_data);
                color_info.resolve_texture = resolve_tex_data->texture;
                color_info.store_op = SDL_GPU_STOREOP_RESOLVE;
            } else {
                color_info.store_op = SDL_GPU_STOREOP_STORE;
            }
        }
    }

    // begin render pass
    m_render_pass = SDL_BeginGPURenderPass(
        m_cmd_render,
        color_infos.data(),
        color_infos.size(),
        depth_stencil_target ? &depth_stencil_info : nullptr
    );

    return m_render_pass != nullptr;
}

void RenderDeviceSDL::end_render_pass() {
    if (m_render_pass)
        SDL_EndGPURenderPass(m_render_pass);

    m_render_pass = nullptr;
    m_render_pass_target = nullptr;
    m_render_pass_pso = nullptr;
    m_render_pass_viewport = {};
    m_render_pass_scissor = {};
    m_render_pass_index_buffer = {};
    m_render_pass_vertex_buffers.clear();
}

void RenderDeviceSDL::reset_command_buffers() {
    EMBER_ASSERT(m_cmd_render == nullptr && m_cmd_transfer == nullptr);

    m_cmd_render = SDL_AcquireGPUCommandBuffer(m_gpu);
    m_cmd_transfer = SDL_AcquireGPUCommandBuffer(m_gpu);

    m_texture_staging_offset = 0;
    m_texture_staging_cycle = 0;
    m_buffer_staging_offset = 0;
    m_buffer_staging_cycle = 0;
}

void RenderDeviceSDL::begin_copy_pass() {
    // only begin a new pass if we don't already have one
    if (m_copy_pass != nullptr)
        return;

    m_copy_pass = SDL_BeginGPUCopyPass(m_cmd_transfer);
}

void RenderDeviceSDL::end_copy_pass() {
    // only end if we have a valid copy pass
    if (m_copy_pass != nullptr) {
        SDL_EndGPUCopyPass(m_copy_pass);
        m_copy_pass = nullptr;
    }
}

void RenderDeviceSDL::flush_commands(bool reset_buffers) {
    end_copy_pass();
    end_render_pass();
    SDL_SubmitGPUCommandBuffer(m_cmd_transfer);
    SDL_SubmitGPUCommandBuffer(m_cmd_render);
    m_cmd_render = nullptr;
    m_cmd_transfer = nullptr;

    if (reset_buffers)
        reset_command_buffers();
}

std::vector<RenderDeviceSDL::TargetAttachmentInfo> RenderDeviceSDL::get_target_formats_and_sample_count(Target* target) {
	// Use the default framebuffer if the provided target is null.
	auto t = target ? target : m_framebuffer.get();
	EMBER_ASSERT(t != nullptr);

	// Pre-allocate formats vector to correct size.
	std::vector<TargetAttachmentInfo> formats;
	formats.reserve(t->attachments().size());

	// Collect format and sample count from each attachment
	for (const auto& handle : t->attachments()) {
		auto tex = m_textures.get(handle);
		EMBER_ASSERT(tex != nullptr);
		formats.emplace_back(tex->format, tex->sample_count);
	}

	return formats;
}
