#pragma once

#include <ember/app/main.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <ember/input/input.h>
#include <ember/memory/memory.h>
#include <ember/platform/window.h>
#include <ember/render/renderer.h>

namespace ember
{
	class Runtime;

	struct UpdateContext
	{
		f32 dt			= 0.0f;
		u64 frame_index = 0;
	};

	struct RenderContext
	{
		f32 dt			= 0.0f;
		u64 frame_index = 0;
		u32 frame_slot	= 0;

		TextureHandle backbuffer   = {};
		Extent2D backbuffer_extent = {};
	};

	struct AppConfig
	{
		MemoryConfig memory			  = {};
		WindowDef window			  = {};
		gpu::DeviceDef gpu			  = {};
		gpu::PresentMode present_mode = gpu::PresentMode::VSync;
		f32 max_delta_seconds		  = 0.1f;
	};

	/**
	 * User-defined application.
	 *
	 * Runtime binds the application only after the complete derived object
	 * has been constructed. Engine resources should therefore be created in
	 * on_init(), not in the game constructor.
	 */
	class App
	{
	public:
		virtual ~App() noexcept = default;

		App(const App&)			   = delete;
		App& operator=(const App&) = delete;
		App(App&&)				   = delete;
		App& operator=(App&&)	   = delete;

		/**
		 * Applications inherit this default, so configuration is optional.
		 *
		 * Configuration runs before the engine allocator exists. Keep it to
		 * lightweight policy values and do not load assets here.
		 */
		[[nodiscard]] static AppConfig configure(const Args&) noexcept { return {}; }

	protected:
		App() noexcept = default;

		[[nodiscard]] const Args& args() const noexcept;
		[[nodiscard]] Platform& platform() noexcept;
		[[nodiscard]] gpu::Device& gpu() noexcept;
		[[nodiscard]] render::Renderer& renderer() noexcept;
		[[nodiscard]] WindowHandle window() const noexcept;
		[[nodiscard]] const Input& input() const noexcept;
		[[nodiscard]] SwapchainHandle swapchain() const noexcept;

		void quit(int exit_code = 0) noexcept;

	private:
		friend class Runtime;

		/**
		 * Returning false still invokes on_shutdown(). This lets applications
		 * release resources created before a later initialization stage failed.
		 */
		virtual bool init() noexcept { return true; }

		virtual void update(const UpdateContext&) noexcept {}

		virtual void render(const RenderContext&) noexcept;

		virtual void shutdown() noexcept {}

		// Runtime is constructed before the application and destroyed after it.
		Runtime* m_runtime = nullptr;
	};
}
