#pragma once

#include "core/common.h"
#include "core/handle.h"
#include "graphics/color.h"
#include "graphics/enums/sample_count.h"
#include "graphics/texture.h"
#include "graphics/target.h"
#include "graphics/shader.h"
#include "graphics/buffer.h"
#include "graphics/enums/clear_mask.h"

namespace Ember
{
	/// Forward Declarations
	class Window;
	struct DrawCommand;

	struct ClearInfo
	{
		std::optional<Color> color;
		std::optional<float> depth;
		std::optional<int> stencil;
		ClearMask mask = ClearMask::All;
	};

	// Struct defining a Texture
	struct TextureDef
	{
		glm::uvec2 size = { 1, 1 };
        TextureFormat format = TextureFormat::Color;
        SampleCount sample_count = SampleCount::One;
        std::span<u8> data;
        bool is_target_attachment = false;
	};

	class RenderDevice
	{
	public:
		/// @brief Initializes the RenderDevice for the current platform
		static void init(Window& window);

		/// @brief Disposes of the RenderDevice
		static void dispose();

		/// @brief Accesses the singleton RenderDevice instance
		static RenderDevice* instance();

		/// Virtual destructor
		virtual	~RenderDevice() = default;

		/**
		 * @brief Clears the provided RenderTarget, defaults to clearing the swapchain if no target is provided.
		 * @param clear_info ClearInfo struct containing the clear metadata.
		 * @param target RenderTarget that should be cleared (or null handle)
		 */
        virtual void clear(ClearInfo clear_info, Ref<Target> target = nullptr) = 0;

        /// @brief Blocks the thread until the GPU has completed all in-flight work.
        virtual void wait_idle() = 0;

        /**
         * @brief Submits a DrawCommand to the GPU.
         * @param cmd DrawCommand struct containing draw data.
         */
        virtual void submit(const DrawCommand& cmd) = 0;

        /// @brief Renders the next available swapchain image to the screen.
        virtual void present() = 0;

		/**
		 * @brief Allocates a Texture on the GPU.
		 * @param def TextureDef struct containing texture configuration.
		 * @returns Typed Handle pointing towards the GPU texture.
		 */
		virtual Handle<Texture> create_texture(const TextureDef& def) = 0;

		/**
         * @brief Updates pixel data of the texture the handle points to (if valid) on the GPU.
         * @param handle Handle<Texture> to be written to.
         * @param data span of pixel data.
         */
        virtual void set_texture_data(Handle<Texture> handle, std::span<u8> data) = 0;

		/**
		 * @brief Deallocates a Texture on the GPU.
		 * @param handle Handle<Texture> of the Texture to deallocate.
		 */
		virtual void dispose_texture(Handle<Texture> handle) = 0;

		/**
		 * @brief Allocates a Shader (PSO) on the GPU.
		 * @param def ShaderDef struct containing the pipeline state configuration.
		 * @returns Typed Handle pointing towards the GPU texture.
		 */
		virtual Handle<Shader> create_shader(const ShaderDef& def) = 0;

		/**
		 * @brief Returns a handle to the default white Texture.
		 */
		virtual Handle<Texture> default_texture() const = 0;

		/**
		 * @brief Deallocates a Shader (PSO) on the GPU.
		 * @param handle Handle<Shader> of the Shader to deallocate.
		 */
		virtual void dispose_shader(Handle<Shader> handle) = 0;

		/**
		 * Allocates a Buffer on the GPU.
		 * @param def BufferDef struct containing buffer configuration.
		 * @returns Typed Handle pointing to the GPU buffer.
		 */
		virtual Handle<Buffer> create_buffer(const BufferDef& def) = 0;

		/**
		 * @brief Updates data of the buffer the handle points to (if valid).
		 * @param handle Handle<Buffer> to be written to.
		 * @param data span of the data.
		 * @param offset Destination offset in bytes.
		 */
		virtual void set_buffer_data(Handle<Buffer> handle, std::span<const std::byte> data, u32 offset) = 0;

		/**
		 * @brief Deallocates a Buffer on the GPU.
		 * @param handle Handle<Buffer> of the Buffer to deallocate.
		 */
		virtual void dispose_buffer(Handle<Buffer> handle) = 0;

		/**
		 * @brief Returns the framebuffer Target.
		 */
		virtual Target& framebuffer() const = 0;

	private:
		static Unique<RenderDevice> s_instance;
	};
}
