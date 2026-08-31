#pragma once

#include <ember/gpu/common.h>
#include <ember/gpu/device.h>
#include <ember/gpu/swapchain.h>
#include <ember/input/input.h>
#include <ember/memory/memory.h>
#include <ember/platform/platform.h>
#include <ember/platform/window.h>

#include <chrono>

namespace ember
{
	struct AppDef
	{
		MemoryConfig memory			  = {};
		gpu::DeviceDef gpu			  = {};
		gpu::PresentMode present_mode = gpu::PresentMode::VSync;

		/// Boots Dear ImGui
		bool enable_imgui = true;

		/// Upper bound on Frame::dt in seconds; hitches and debugger stalls clamp here.
		f32 max_dt = 0.1f;
	};

	/// One frame as the game sees it. Fields hold until the next next_frame().
	struct Frame
	{
		f32 dt	  = 0.0f; // seconds, clamped to AppDef::max_dt
		u32 index = 0;	  // monotonic
		u32 slot  = 0;	  // index % max_frames_in_flight, for per slot game state

		Extent2D extent			 = {};
		TextureHandle backbuffer = {}; // null when unpresentable (minimized): skip drawing, keep simulating
	};

	/**
	 * Boots the engine stack and pumps frames; the game keeps main and the loop.
	 * Accessors hand out the real modules, App owns wiring and lifetime.
	 *
	 * Member declaration order is the boot order, Reverse destruction is the teardown
	 * order, so a partially booted App unwinds on its own.
	 */
	class App final
	{
	public:
		explicit App(const AppDef& def) noexcept;
		~App() noexcept;

		App(const App&)			   = delete;
		App& operator=(const App&) = delete;
		App(App&)				   = delete;
		App& operator=(App&&)	   = delete;

		/// False when any boot stage failed; the stage has already logged why.
		[[nodiscard]] explicit operator bool() const noexcept { return m_valid; }

		/**
		 * Closes the previous frame and opens the next: device frame, event pump,
		 * clock, backbuffer acquire, UI frame. Returns false once quit was requested,
		 * after closing the final frame.
		 *
		 * Leaving the loop mid frame is legal; the destructor closes it, and the
		 * device presents a fallback clear for frames nothing drew to.
		 */
		[[nodiscard]] bool next_frame() noexcept;

		[[nodiscard]] const Frame& frame() const noexcept { return m_frame; }

		/// Takes effect at the next next_frame() call.
		void request_quit() noexcept { m_quit = true; }

		[[nodiscard]] Platform& platform() noexcept { return m_platform; }
		[[nodiscard]] gpu::Device& gpu() noexcept { return m_gpu; }
		[[nodiscard]] Input& input() noexcept { return m_input; }
		[[nodiscard]] WindowHandle window() const noexcept { return m_window; }
		[[nodiscard]] SwapchainHandle swapchain() const noexcept { return m_swapchain; }

	private:
		void close_frame() noexcept;

		MemorySystem m_memory;
		Platform m_platform;
		gpu::Device m_gpu;
		Input m_input;

		WindowHandle m_window;
		SwapchainHandle m_swapchain;

		Frame m_frame;
		std::chrono::steady_clock::time_point m_previous_tick;
		f32 m_max_dt	  = 0.1f;
		bool m_imgui	  = false;
		bool m_frame_open = false;
		bool m_quit		  = false;
		bool m_valid	  = false;
	};
}
