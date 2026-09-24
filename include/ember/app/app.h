#pragma once

#include <ember/app/main.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <ember/input/input.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>
#include <ember/platform/window.h>
#include <ember/render/renderer.h>

namespace ember
{
	class Runtime;

	/**
	 * One frame as the app sees it, handed to update() and then to render(). A frame is a
	 * piece of data, not a length of time. Everything a stage needs is in here or reachable
	 * from here.
	 *
	 * The arenas are the frame's memory lifetimes, freed by tag, never per allocation.
	 *
	 * Each stage allocates only from the ones it owns:
	 * 		sim_scratch 	update() only						Gone once update() returns.
	 * 		sim_to_render	update() writes, render() reads.	Gone once the frame has been rendered.
	 *		render_scratch 	render() only. 						Gone oncew the frame has been submitted.
	 */
	struct FrameParams
	{
		u64 frame_index = 0;
		u32 frame_slot	= 0;
		f32 dt			= 0.0f;

		TextureHandle backbuffer   = {};
		Extent2D backbuffer_extent = {};

		Arena& sim_scratch;	   // MemoryLifetime::SimScratch
		Arena& sim_to_render;  // MemoryLifetime::SimToRender
		Arena& render_scratch; // MemoryLifetime::RenderScratch
	};

	struct AppConfig
	{
		MemoryConfig memory			  = {};
		jobs::JobSystemDef jobs		  = {};
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

		virtual void update(const FrameParams&) noexcept {}

		virtual void render(const FrameParams&) noexcept {}

		virtual void shutdown() noexcept {}

		// Runtime is constructed before the application and destroyed after it.
		Runtime* m_runtime = nullptr;
	};
}
