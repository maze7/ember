#pragma once

#include "SDL3/SDL_gpu.h"
#include "core/common.h"
#include "core/pool.h"
#include "math/rect.h"
#include "graphics/render_device.h"

namespace Ember
{
	// Internal platform-specific Texture data.
	struct TextureSDL
	{
		SDL_GPUTexture*         texture = nullptr;
		SDL_GPUTextureFormat    format;
		glm::uvec2	            size = { 0, 0 };
		bool                    is_target_attachment = false;
		SDL_GPUSampleCount 		sample_count = SDL_GPU_SAMPLECOUNT_1;
		Handle<Texture> 		msaa_resolve_texture = Handle<Texture>::null;
	};

	// Internal platform-specific Shader (PSO) data.
	struct ShaderSDL
	{
	    SDL_GPUShader* vertex = nullptr;
		SDL_GPUShader* fragment = nullptr;
		std::vector<u64> pso_hashes;
	};

	// Internal platform-specific Buffer.
	struct BufferSDL
	{
		SDL_GPUBuffer* buffer;
		SDL_GPUBufferUsageFlags usage;
		u32 size;
		bool dirty;
	};

	class RenderDeviceSDL : public RenderDevice
	{
	public:
		/// @brief Maximum number of frames that can be in flight at any given time.
		static constexpr u32 MAX_FRAMES_IN_FLIGHT = 3;

		/// @brief Maximum number of color attachments
		static constexpr u32 MAX_COLOR_ATTACHMENTS = 8;

		/**
		 * @brief Constructs an SDL RenderDevice. Performs all required initialization prior to any rendering.
		 * @param window Window instance that will be used for rendering.
		 */
		explicit RenderDeviceSDL(Window& window);

		/** @brief Deconstructs the SDL RenderDevice, cleaning up any allocated GPU resources in the process. */
		~RenderDeviceSDL() override;

		/**
		 * @brief Clears the provided RenderTarget, defaults to clearing the swapchain if no target is provided.
		 * @param clear_info ClearInfo struct containing the clear metadata.
		 * @param target RenderTarget that should be cleared (or null handle)
		 */
		void clear(ClearInfo clear_info, Ref<Target> target = nullptr) override;

		/// @brief Blocks the thread until the GPU has completed all in-flight work.
		void wait_idle() override;

		/**
         * @brief Submits a DrawCommand to the GPU.
         * @param cmd DrawCommand struct containing draw data.
         */
        void submit(const DrawCommand& cmd) override;

		/// @brief Renders the next available swapchain image to the screen.
		void present() override;

		/**
		 * @brief Allocates a Texture on the GPU.
		 * @param def TextureDef struct containing texture configuration.
		 * @returns Typed Handle pointing towards the GPU texture.
		 */
    	Handle<Texture> create_texture(const TextureDef& def) override;

        /**
         * @brief Updates pixel data of the texture the handle points to (if valid) on the GPU.
         * @param handle Handle<Texture> to be written to.
         * @param data span of pixel data.
         */
        void set_texture_data(Handle<Texture> handle, std::span<u8> data) override;

        /**
		 * @brief Deallocates a Texture on the GPU.
		 * @param handle Handle<Texture> of the Texture to deallocate.
		 */
		void dispose_texture(Handle<Texture> handle) override;

		/**
		 * @brief Returns a Handle to the default white Texture.
		 */
		virtual Handle<Texture> default_texture() const override { return m_default_texture; }

		/**
		 * @brief Allocates a Shader (PSO) on the GPU.
		 * @param def ShaderDef struct containing the pipeline state configuration.
		 * @returns Typed Handle pointing towards the GPU texture.
		 */
		Handle<Shader> create_shader(const ShaderDef& def) override;

		/**
		 * @brief Deallocates a Shader (PSO) on the GPU.
		 * @param handle Handle<Shader> of the Shader to deallocate.
		 */
		void dispose_shader(Handle<Shader> handle) override;

		/**
		 * Allocates a Buffer on the GPU.
		 * @param def BufferDef struct containing buffer configuration.
		 * @returns Typed Handle pointing to the GPU buffer.
		 */
		Handle<Buffer> create_buffer(const BufferDef& def) override;

		/**
		 * @brief Updates data of the buffer the handle points to (if valid).
		 * @param handle Handle<Buffer> to be written to.
		 * @param data span of the data.
		 * @param offset Destination offset in bytes.
		 */
		void set_buffer_data(Handle<Buffer> handle, std::span<const std::byte> data, u32 offset) override;

		/**
		 * @brief Deallocates a Buffer on the GPU.
		 * @param handle Handle<Buffer> of the Buffer to deallocate.
		 */
		void dispose_buffer(Handle<Buffer> handle) override;

		/**
		 * @brief Returns the framebuffer Target.
		 */
		Target& framebuffer() const override { return *m_framebuffer; }

	private:
	    static constexpr u32 STAGING_BUFFER_SIZE = 16 * 1024 * 1024; // 16mb
		static constexpr u32 MAX_STAGING_CYCLE_COUNT = 4;

	    /**
		 * @brief Resets and re-acquires the render and transfer command buffers.
		 * @note This should be called at the start of each frame or before a new set of commands.
		 */
        void reset_command_buffers();

        /**
         * @brief Flushes all commands recorded
         * @param reset_buffers If true, all command buffers will be reset
         */
        void flush_commands(bool reset_buffers);
  		void flush_commands_and_acquire_fences();
		void flush_commands_and_stall();

        /**
         * @brief Begins a render pass on the provided RenderTarget.
         * @note If no target is provided, the swapchain backbuffer is used.
         * @param clear ClearInfo to be used to clear the RenderTarget
         * @param target RenderTarget to be cleared
         * @returns True if a render pass was begun, or an existing one is usable for the RenderTarget
         */
        bool begin_render_pass(ClearInfo clear, Target* target = nullptr);

        /// @brief Ends the current render pass, if one exists.
        void end_render_pass();

        /// @brief Begins a copy pass (if one is not already in progress)
        void begin_copy_pass();

        /// @brief Ends a copy pass (if one has been started)
        void end_copy_pass();

        /// @brief Retrieves (or creates) a PSO from the cache.
        SDL_GPUGraphicsPipeline* get_pipeline(const DrawCommand& cmd);

        /// @brief Retrieves (or creates) a SDL_GPUSampler*
        SDL_GPUSampler* get_sampler(TextureSampler sampler);

        /// @brief Retrieves the format and sample count for each Target attachment
        struct TargetAttachmentInfo { SDL_GPUTextureFormat format; SDL_GPUSampleCount sample_count; };
        std::vector<TargetAttachmentInfo> get_target_formats_and_sample_count(Target* target);

		/// @brief Instance of the Window that will be used for rendering.
		Window& m_window;

		/// @brief SDL_gpu device instance, used to interface with the underlying GPU API.
		SDL_GPUDevice* m_gpu = nullptr;

		// @brief Texture pool
		Pool<TextureSDL, Texture> m_textures;
		Pool<ShaderSDL, Shader> m_shaders;
		Pool<BufferSDL, Buffer> m_buffers;

		/// Staging buffers
		SDL_GPUTransferBuffer* m_texture_staging = nullptr;
		SDL_GPUTransferBuffer* m_buffer_staging = nullptr;

		// Command buffers
		SDL_GPUCommandBuffer* m_cmd_render = nullptr;
		SDL_GPUCommandBuffer* m_cmd_transfer = nullptr;

		// Render / Copy Passes
		SDL_GPUCopyPass* m_copy_pass = nullptr;
		SDL_GPURenderPass* m_render_pass = nullptr;
		SDL_GPUGraphicsPipeline* m_render_pass_pso = nullptr;

		// Frame synchronization
		u32 m_frame = 0;
		SDL_GPUFence* m_fences[MAX_FRAMES_IN_FLIGHT][2] = {
			{ nullptr, nullptr },
			{ nullptr, nullptr },
		};

		// Render pass & Render Target
		Ref<Target> m_framebuffer = nullptr;
		Handle<Texture> m_default_texture = Handle<Texture>::null;
		Target* m_render_pass_target = nullptr;
		Recti m_render_pass_viewport;
		Recti m_render_pass_scissor;
		Handle<Buffer> m_render_pass_index_buffer;
	    std::vector<Handle<Buffer>> m_render_pass_vertex_buffers;

		// Staging buffer tracking variables
		u32 m_texture_staging_offset = 0;
		u32 m_texture_staging_cycle = 0;
		u32 m_buffer_staging_offset = 0;
		u32 m_buffer_staging_cycle = 0;

		// Hash for cached (on-demand) data.
		std::unordered_map<TextureSampler, SDL_GPUSampler*> m_samplers;
		std::unordered_map<u64, SDL_GPUGraphicsPipeline*> m_psos;
	};
}
