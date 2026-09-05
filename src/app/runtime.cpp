#include <ember/app/runtime.h>
#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/memory/pmr/arena_resource.h>

#include <algorithm>
#include <thread>

namespace ember
{
	Runtime::Runtime(const AppConfig& config, const Args& args) noexcept
		: m_memory(config.memory), m_platform(), m_gpu(m_platform, config.gpu), m_args(args),
		  m_max_delta_seconds(config.max_delta_seconds)
	{
		if (m_max_delta_seconds <= 0.0f)
		{
			EMBER_ERROR("(ember::Runtime): max_delta_seconds must be greater than zero");
			return;
		}

		if (!m_memory || !m_platform || !m_gpu)
			return;

		m_window = m_platform.create_window(config.window);
		if (m_window.is_null())
		{
			EMBER_ERROR("(ember::Runtime): window creation failed");
			return;
		}

		// Wirte Platform up to the GPU device so it can access window info
		m_swapchain = m_gpu.create_swapchain({
			.window		  = m_window,
			.present_mode = config.present_mode,
		});

		if (m_swapchain.is_null())
		{
			EMBER_ERROR("runtime: swapchain creation failed");
			return;
		}

		m_valid = true;
	}

	Runtime::~Runtime() noexcept
	{
		close_frame();

		if (m_gpu)
		{
			// Application resources are already gone when Runtime is destroyed.
			m_gpu.wait_idle();

			if (!m_swapchain.is_null())
			{
				m_gpu.destroy(m_swapchain);
				m_swapchain = {};
				m_gpu.wait_idle();
			}
		}

		if (!m_window.is_null())
		{
			m_platform.destroy_window(m_window);
			m_window = {};
		}
	}

	Runtime::FrameScope::FrameScope(Runtime& runtime) noexcept : m_runtime(runtime), m_info(runtime.m_gpu.begin_frame())
	{
		EMBER_ASSERT(!runtime.m_frame_open);
		runtime.m_frame_open = true;
	}

	Runtime::FrameScope::~FrameScope() noexcept { m_runtime.close_frame(); }

	int Runtime::run(App& app) noexcept
	{
		if (!m_valid)
			return 1;

		EMBER_ASSERT(app.m_runtime == nullptr);
		app.m_runtime = this;

		bool initialized = app.init();

		// Initialization work must not become the first simulation delta.
		m_previous_frame = std::chrono::steady_clock::now();

		if (!initialized)
		{
			app.shutdown();

			if (m_exit_code == 0)
				m_exit_code = 1;

			return m_exit_code;
		}

		while (!m_quit_requested)
		{
			memory::frame_arena().reset();

			if (m_platform.pump_events(m_input).quit_requested)
			{
				m_quit_requested = true;
				break;
			}

			auto tick = std::chrono::steady_clock::now();
			f32 dt = std::clamp(std::chrono::duration<f32>(tick - m_previous_frame).count(), 0.0f, m_max_delta_seconds);
			m_previous_frame = tick;

			UpdateContext update{
				.dt			 = dt,
				.frame_index = m_frame_index++,
			};

			app.update(update);

			if (m_quit_requested)
				break;

			auto pixels = m_platform.window_pixel_size(m_window);
			if (pixels.x == 0 || pixels.y == 0)
			{
				// A suspended window should not consume an entire CPU core.
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
				continue;
			}

			FrameScope frame(*this);
			TextureHandle output = m_gpu.acquire(m_swapchain);
			if (output.is_null())
				continue;

			RenderContext render{
				.dt				   = dt,
				.frame_index	   = frame.info().frame_index,
				.frame_slot		   = frame.info().slot,
				.backbuffer		   = output,
				.backbuffer_extent = m_gpu.swapchain_extent(m_swapchain),
			};

			app.render(render);
			if (m_gpu.device_lost())
			{
				EMBER_ERROR("(ember::Runtime): GPU device lost");
				m_exit_code		 = 1;
				m_quit_requested = true;
			}
		}

		app.shutdown();
		return m_exit_code;
	}

	void Runtime::request_quit(int exit_code) noexcept
	{
		m_exit_code		 = exit_code;
		m_quit_requested = true;
	}

	void Runtime::close_frame() noexcept
	{
		if (!m_frame_open)
			return;

		m_frame_open = false;
		m_gpu.end_frame();
	}
}
