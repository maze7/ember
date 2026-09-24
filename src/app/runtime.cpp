#include <ember/app/runtime.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>
#include <ember/gpu/device.h>
#include <ember/memory/memory.h>
#include <ember/platform/platform.h>

#include <chrono>
#include <thread>

namespace ember
{
	Runtime::~Runtime() noexcept { shutdown(); }

	Result<void, RuntimeError> Runtime::initialize(const AppConfig& config, const Args& args) noexcept
	{
		if (m_state != State::Empty)
			return fail(RuntimeError::AlreadyInit);

		m_state				= State::Initializing;
		m_args				= args;
		m_max_delta_seconds = config.max_delta_seconds;

		const auto rollback = [this](RuntimeError error) -> Result<void, RuntimeError>
		{
			shutdown();
			return fail(error);
		};

		if (!memory::initialize(config.memory))
			return rollback(RuntimeError::MemoryInitFailed);

		// The frame lifetimes, one arena each on the shared heap. Sequence 0 is the boot frame:
		// whatever initialization allocates from them lives until the first frame begins.
		{
			TaggedHeap& heap = memory::tagged_heap();

			m_sim_scratch.init(heap, "game_scratch");
			m_sim_to_render.init(heap, "game_to_render");
			m_render_scratch.init(heap, "render_scratch");

			m_sim_scratch.begin(heap_tag(MemoryLifetime::SimScratch, 0));
			m_sim_to_render.begin(heap_tag(MemoryLifetime::SimToRender, 0));
			m_render_scratch.begin(heap_tag(MemoryLifetime::RenderScratch, 0));
		}

		jobs::initialize(config.jobs);

		m_platform = memory::make_unique<Platform>(MemoryTag::Engine);
		if (!m_platform)
			return rollback(RuntimeError::PlatformInitFailed);

		m_window = m_platform->create_window(config.window);
		if (m_window.is_null())
			return rollback(RuntimeError::WindowInitFailed);

		m_gpu = memory::make_unique<gpu::Device>(MemoryTag::Graphics, *m_platform, config.gpu);
		if (!m_gpu)
			return rollback(RuntimeError::DeviceInitFailed);

		m_swapchain = m_gpu->create_swapchain({
			.window		  = m_window,
			.present_mode = config.present_mode,
		});

		if (m_swapchain.is_null())
			return rollback(RuntimeError::SwapchainInitFailed);

		// Initialize the renderer after the GPU is available.
		m_renderer = memory::make_unique<render::Renderer>(MemoryTag::Graphics);
		m_renderer->init(*m_gpu, {});
		m_state = State::Ready;

		return {};
	}

	void Runtime::shutdown() noexcept
	{
		// Submitted frames may still reference renderer-owned resources,
		// so quiesce the GPU before tearing the renderer down.
		if (m_gpu)
			m_gpu->wait_idle();

		if (m_renderer)
		{
			m_renderer->shutdown(*m_gpu);
			m_renderer.reset();
		}

		if (m_gpu)
		{
			if (!m_swapchain.is_null())
				m_gpu->destroy(m_swapchain);

			m_swapchain = {};

			// Resource destruction is deferred. Drain the retirement queue before
			// destroying the device that owns it.
			m_gpu->wait_idle();
			m_gpu.reset();
		}

		if (m_platform)
		{
			if (!m_window.is_null())
				m_platform->destroy_window(m_window);

			m_window = {};
			m_platform.reset();
		}

		// Worker teardown may still touch engine allocators, so stop the scheduler before
		// releasing the memory system. The lifetimes go between the two: each arena frees
		// whatever tag it still holds, and the heap asserts on blocks still owned.
		jobs::shutdown();
		m_render_scratch.shutdown();
		m_sim_to_render.shutdown();
		m_sim_scratch.shutdown();
		m_input.clear();
		memory::shutdown();

		m_args				= {};
		m_previous_frame	= {};
		m_max_delta_seconds = 0.1f;
		m_frame_index		= 0;
		m_exit_code			= 0;
		m_quit_requested	= false;
		m_state				= State::Empty;
	}

	bool Runtime::initialized() const noexcept { return m_state == State::Ready || m_state == State::Running; }

	int Runtime::run(App& app) noexcept
	{
		EMBER_ASSERT(m_state == State::Ready && "Runtime::run() requires successful initialization");
		EMBER_ASSERT(app.m_runtime == nullptr && "App is already bound to a Runtime");

		if (m_state != State::Ready || app.m_runtime != nullptr)
			return 1;

		m_exit_code		 = 0;
		m_frame_index	 = 0;
		m_quit_requested = false;
		m_previous_frame = {};
		m_state			 = State::Running;
		app.m_runtime	 = this;

		frame_loop(app);

		app.m_runtime = nullptr;
		m_state		  = State::Ready;

		return m_exit_code;
	}

	void Runtime::frame_loop(App& app) noexcept
	{
		const bool app_initialized = app.init();

		// Initialization work must not contribute to the first simulation step.
		m_previous_frame = std::chrono::steady_clock::now();

		if (!app_initialized)
		{
			app.shutdown();

			if (m_exit_code == 0)
				m_exit_code = 1;

			return;
		}

		while (!m_quit_requested)
		{
			// Last frame's lifetimes die here, three bulk frees by tag, and this frame's are named
			// after it (sequence 0 was boot). The loop is serial: every job that allocated under the
			// old tags has joined and every staged copy out of them has been recorded, so all three
			// retire at one point. The overlapped loop moves the frees apart, game scratch at the
			// join and the other two after end_frame, without changing what the tags mean.
			{
				TaggedHeap& heap   = memory::tagged_heap();
				const u64 sequence = m_frame_index + 1;

				heap.free(m_sim_scratch.end());
				heap.free(m_sim_to_render.end());
				heap.free(m_render_scratch.end());

				m_sim_scratch.begin(heap_tag(MemoryLifetime::SimScratch, sequence));
				m_sim_to_render.begin(heap_tag(MemoryLifetime::SimToRender, sequence));
				m_render_scratch.begin(heap_tag(MemoryLifetime::RenderScratch, sequence));
			}

			// Platform event pump
			{
				EMBER_PROFILE_SCOPE_C("pump events", PROFILE_COLOR_INPUT);
				if (m_platform->pump_events(m_input).quit_requested)
					request_quit(0);
			}

			if (m_quit_requested)
				break;

			// A breakpoint or long hitch should not become an unbounded simulation step.
			auto tick = std::chrono::steady_clock::now();
			f32 dt = std::clamp(std::chrono::duration<f32>(tick - m_previous_frame).count(), 0.0f, m_max_delta_seconds);

			m_previous_frame = tick;

			FrameParams frame_params{
				.frame_index	= m_frame_index++,
				.dt				= dt,
				.sim_scratch	= m_sim_scratch,
				.sim_to_render	= m_sim_to_render,
				.render_scratch = m_render_scratch,
			};

			{
				EMBER_PROFILE_SCOPE_C("update", PROFILE_COLOR_GAMEPLAY);
				app.update(frame_params);
			}

			if (m_quit_requested)
				break;

			Extent2D pixels = m_platform->window_pixel_size(m_window);
			if (pixels.width == 0 || pixels.height == 0)
			{
				// A minimzed window has no drawable. Sleep to avoid a hot loop
				// while continuing to poll for restore events.
				std::this_thread::sleep_for(std::chrono::milliseconds(16));

				EMBER_PROFILE_FRAME();
				continue;
			}

			const gpu::FrameInfo frame = m_gpu->begin_frame();
			const TextureHandle output = m_gpu->acquire(m_swapchain);

			if (!output.is_null())
			{
				frame_params.frame_slot		   = frame.slot;
				frame_params.backbuffer		   = output;
				frame_params.backbuffer_extent = m_gpu->swapchain_extent(m_swapchain);

				EMBER_PROFILE_SCOPE_C("render", PROFILE_COLOR_RENDER);
				app.render(frame_params);
			}

			// No control-flow statement may bypass this after begin_frame().
			m_gpu->end_frame();

			if (m_gpu->device_lost())
			{
				EMBER_ERROR("GPU device lost");
				request_quit(1);
			}

			EMBER_PROFILE_FRAME();
		}

		app.shutdown();
	}

	void Runtime::request_quit(int exit_code) noexcept
	{
		m_exit_code		 = exit_code;
		m_quit_requested = true;
	}
}
