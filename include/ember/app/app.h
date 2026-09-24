#pragma once

#include <ember/app/frame.h>
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
		[[nodiscard]] SwapchainHandle swapchain() const noexcept;

		/**
		 * One of the last FrameRing::CAPACITY frames by number, the current one included: what an
		 * earlier stage saw and measured. Null for a frame older than that or not begun yet.
		 */
		[[nodiscard]] const FrameParams* frame(u64 index) const noexcept;

		/**
		 * True once the GPU has retired everything frame index submitted, or the frame is older
		 * than the ring remembers. False for the current frame and every frame after it. Data the
		 * GPU reads for a frame must stay put until this says so.
		 */
		[[nodiscard]] bool is_frame_complete(u64 index) const noexcept;

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
