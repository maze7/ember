#include <ember/app/app.h>
#include <ember/core/logger.h>
#include <ember/imgui/imgui_backend.h>

#include <chrono>

namespace ember
{
	namespace
	{
		[[nodiscard]] gpu::DeviceDef wire(gpu::DeviceDef def, Platform& platform) noexcept
		{
			def.platform = &platform;
			return def;
		}
	}

	App::App(const AppDef& def) noexcept : m_memory(def.config), m_gpu(wire(def.gpu, m_platform)), m_max_dt(def.max_dt)
	{
		if (!m_memory || !m_platform || !m_gpu)
			return;

		m_window = m_platform.create_window(def.window);
		if (m_window.is_null())
		{
			EMBER_ERROR("app: window creation failed");
			return;
		}

		m_swapchain = m_gpu.create_swapchain({.window = m_window, .present_mode = def.present_mode});
		if (m_swapchain.is_null())
			return;

		if (def.imgui_shader.empty())
		{
			EMBER_ERROR("app: AppDef::imgui_shader is empty; pass the cooked imgui.spv");
			return;
		}

		const imgui::BackendDef imgui_def{
			.shader		  = def.imgui_shader,
			.color_format = m_gpu.swapchain_format(m_swapchain),
		};

		if (!imgui::init(m_gpu, m_platform, imgui_def))
			return;

		m_previous_tick = std::chrono::steady_clock::now();
		m_valid			= true;
	}

	App::~App() noexcept
	{
		if (m_gpu)
		{
			close_frame();
			m_gpu.wait_idle();
		}

		imgui::shutdown(m_gpu);

		if (!m_swapchain.is_null())
			m_gpu.destroy(m_swapchain);

		if (!m_window.is_null())
			m_platform.destroy_window(m_window);
	}

	const Frame& App::next_frame() noexcept
	{
		close_frame();

		if (!m_valid || m_quit)
		{
			m_frame				   = {};
			m_frame.quit_requested = true;
			return m_frame;
		}

		const gpu::FrameInfo info = m_gpu.begin_frame();
		m_frame_open			  = true;

		if (m_platform.pump_events(m_input).quit_requested)
			m_quit = true;

		// Nothing was acquired yet, so closing here presents nothing.
		if (m_quit)
		{
			close_frame();
			m_frame				   = {};
			m_frame.quit_requested = true;
			return m_frame;
		}

		const auto tick		   = std::chrono::steady_clock::now();
		m_frame.dt			   = std::clamp(std::chrono::duration<f32>(tick - m_previous_tick).count(), 0.0f, m_max_dt);
		m_previous_tick		   = tick;
		m_frame.index		   = info.frame_index;
		m_frame.slot		   = info.slot;
		m_frame.backbuffer	   = m_gpu.acquire(m_swapchain);
		m_frame.extent		   = m_gpu.swapchain_extent(m_swapchain);
		m_frame.quit_requested = false;

		imgui::new_frame(m_input, m_window, m_frame.extent, m_frame.dt);

		return m_frame;
	}

	void App::close_frame() noexcept
	{
		if (!m_frame_open)
			return;

		m_frame_open = false;

		// A UI frame with no backbuffer has nowhere to land
		if (m_frame.backbuffer.is_null())
			imgui::discard();

		m_gpu.end_frame();
	}
}
