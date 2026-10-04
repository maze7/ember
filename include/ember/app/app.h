#pragma once

#include <ember/app/frame.h>
#include <ember/app/main.h>
#include <ember/assets/asset.h>
#include <ember/assets/material_asset.h>
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

	struct AppConfig
	{
		MemoryConfig memory			  = {};
		jobs::JobSystemDef jobs		  = {};
		AssetManagerDef assets		  = {};
		MaterialAssetsDef materials	  = {};
		WindowDef window			  = {};
		gpu::DeviceDef gpu			  = {};
		gpu::PresentMode present_mode = gpu::PresentMode::VSync;
		f32 max_delta_seconds		  = 0.1f;
	};

	/**
	 * User-defined application.
	 *
	 * Runtime binds the application only after the complete derived object has been
	 * constructed. Engine resources should therefore be created in init(), not in the game
	 * constructor.
	 *
	 * A frame is two stages, run one after the other on the owner thread, which holds the
	 * platform and the device: update() moves the game on, then render() draws it. update()
	 * runs every frame; render() only when there is something to draw on, so a game goes on
	 * behind a minimized window. Either stage may use the platform, the device and the UI, on
	 * this thread only: a job a stage kicks has none of them. What update() leaves for
	 * render() stays in the app, or in the frame's memory when it should die with the frame.
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
		[[nodiscard]] SwapchainHandle swapchain() const noexcept;
		[[nodiscard]] AssetManager& assets() noexcept;

		/**
		 * One of the last FrameRing::CAPACITY frames by number, the current one included: what an
		 * earlier stage saw and measured. Null for a frame older than that or not begun yet.
		 */
		[[nodiscard]] const FrameParams* frame(u64 index) const noexcept;

		/**
		 * Asks the loop to stop: any stage, any thread. The frame under way finishes like any
		 * other, render() included, so update() still leaves a frame that can be drawn.
		 */
		void quit(int exit_code = 0) noexcept;

	private:
		friend class Runtime;

		/**
		 * Returning false still invokes on_shutdown(). This lets applications
		 * release resources created before a later initialization stage failed.
		 */
		virtual bool init() noexcept { return true; }

		virtual void update(FrameParams&) noexcept {}

		virtual void render(FrameParams&) noexcept {}

		virtual void shutdown() noexcept {}

		// Runtime is constructed before the application and destroyed after it.
		Runtime* m_runtime = nullptr;
	};
}
