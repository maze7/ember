#include <ember/anim/library.h>
#include <ember/app/runtime.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>
#include <ember/gpu/device.h>
#include <ember/memory/memory.h>
#include <ember/platform/platform.h>

#include <chrono>
#include <thread>

namespace
{
	/// Steady clock nanoseconds for the frame's stage stamps. Only differences mean anything.
	[[nodiscard]] ember::u64 now_ns() noexcept
	{
		return static_cast<ember::u64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
				.count());
	}
}

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
		}

		jobs::initialize(config.jobs);

		m_platform = memory::make_unique<Platform>(MemoryTag::Engine);
		if (!*m_platform)
			return rollback(RuntimeError::PlatformInitFailed);

		m_window = m_platform->create_window(config.window);
		if (m_window.is_null())
			return rollback(RuntimeError::WindowInitFailed);

		m_gpu = memory::make_unique<gpu::Device>(MemoryTag::Graphics, *m_platform, config.gpu);
		if (!*m_gpu)
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

		m_assets = memory::make_unique<AssetManager>(MemoryTag::Assets);
		m_assets->init(*m_gpu, config.assets);

		// The engine's own types come registered, as the manager's textures and the material library's
		// types do. A game registers its own in its init().
		anim::register_types(*m_assets);

		// Materials are assets that load into the renderer's registry, which they reach through this.
		m_materials = memory::make_unique<MaterialAssets>(MemoryTag::Assets);
		m_materials->init(*m_assets, m_renderer->materials(), config.materials);
		m_state = State::Ready;

		return {};
	}

	void Runtime::shutdown() noexcept
	{
		// The material library lets go of what it holds first, and goes last: the manager's unloads
		// reach the registry through it.
		if (m_materials)
			m_materials->shutdown();

		if (m_assets)
		{
			m_assets->shutdown();
			m_assets.reset();
		}

		m_materials.reset();

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
		m_frames.clear();
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
		m_frames.clear();

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

		// A quit asked for during a frame ends the loop here, once that frame has finished.
		while (!quit_requested())
		{
			run_frame(app);
			EMBER_PROFILE_FRAME();
		}

		app.shutdown();
	}

	/**
	 * One frame from start to finish, on this thread, the owner of the platform and the device:
	 *
	 *   wait      for the GPU and the display; the GPU's frame is open from here to the submit
	 *   sample    input, time and the window's size
	 *   update()  the game moves on and publishes what to draw
	 *   render()  draws it
	 *   submit    and the frame's memory goes
	 *
	 * Each lifetime is begun under the frame's tag where its stage starts and freed once its last
	 * reader is done:
	 *
	 *   sim_scratch     begun before update(), freed when it returns
	 *   sim_to_render   begun before update(), closed when it returns, freed after the submit
	 *   render_scratch  begun before render(), freed after the submit
	 */
	void Runtime::run_frame(App& app) noexcept
	{
		TaggedHeap& heap = memory::tagged_heap();

		// The frame's number is its identity: the ring slot it takes and the sequence half of its
		// three lifetime tags. Boot was 0, so the first frame is 1.
		const u64 index = m_frame_index + 1;

		// Between frames: assets nobody holds go, finished reloads fold into their payloads, and
		// quiet file changes become reloads.
		m_assets->pump(index);

		// The waits come before anything the frame samples, so input and time are as fresh as they
		// can be when update() reads them. A minimized window has no drawable and no display to
		// wait for: the sleep stands in, so the loop does not spin while there is nothing to show.
		const Extent2D pixels = m_platform->window_pixel_size(m_window);
		const bool drawable	  = pixels.width != 0 && pixels.height != 0;

		gpu::FrameInfo info		 = {};
		TextureHandle backbuffer = {};

		if (drawable)
		{
			// Waits for the GPU to finish the frame that last used this slot, and names that wait itself.
			info = m_gpu->begin_frame();

			// Waits for the display to hand an image back: under vsync, most of a frame.
			EMBER_PROFILE_SCOPE_C("wait for display", PROFILE_COLOR_WAIT);
			backbuffer = m_gpu->acquire(m_swapchain);
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}

		{
			EMBER_PROFILE_SCOPE_C("pump events", PROFILE_COLOR_INPUT);
			if (m_platform->pump_events(m_input).quit_requested)
				request_quit(0);
		}

		// A breakpoint or long hitch should not become an unbounded simulation step.
		const auto tick = std::chrono::steady_clock::now();
		const f32 dt = std::clamp(std::chrono::duration<f32>(tick - m_previous_frame).count(), 0.0f, m_max_delta_seconds);

		m_previous_frame = tick;

		// The frame begins: its slot takes the number, a copy of the input the pump just published,
		// the window's size, and what it will be drawn on when there is something to draw on.
		FrameParams& frame	= m_frames.begin(index, dt, m_input.state());
		frame.window_extent = m_platform->window_pixel_size(m_window);
		m_frame_index		= index;

		if (!backbuffer.is_null())
		{
			frame.frame_slot		= info.slot;
			frame.backbuffer		= backbuffer;
			frame.backbuffer_extent = m_gpu->swapchain_extent(m_swapchain);
		}

		m_sim_scratch.begin(heap_tag(MemoryLifetime::SimScratch, index));
		m_sim_to_render.begin(heap_tag(MemoryLifetime::SimToRender, index));

		{
			EMBER_PROFILE_SCOPE_C("update", PROFILE_COLOR_GAMEPLAY);
			frame.update_begin_ns = now_ns();
			app.update(frame);
			frame.update_end_ns = now_ns();
		}

		// update() has returned: its scratch dies here, and its packet closes but stays alive
		// until the frame has been submitted.
		heap.free(m_sim_scratch.end());
		const HeapTag packet = m_sim_to_render.end();

		if (!backbuffer.is_null())
		{
			m_render_scratch.begin(heap_tag(MemoryLifetime::RenderScratch, index));

			// Stamped after the waits for the GPU and the display, so render time is work, not waiting.
			EMBER_PROFILE_SCOPE_C("render", PROFILE_COLOR_RENDER);
			frame.render_begin_ns = now_ns();
			app.render(frame);
			frame.render_end_ns = now_ns();
		}

		// The GPU frame closes on every path that opened it. What it submitted is the frame's GPU
		// work, which is_frame_complete() asks after. Every copy out of the frame's memory has
		// been recorded by now, so the render scratch and the packet both die here.
		if (drawable)
			frame.gpu = m_gpu->end_frame();

		if (!backbuffer.is_null())
			heap.free(m_render_scratch.end());

		heap.free(packet);

		if (m_gpu->device_lost())
		{
			EMBER_ERROR("GPU device lost");
			request_quit(1);
		}
	}

	bool Runtime::is_frame_complete(u64 index) const noexcept
	{
		// The newest frame is still under way, its gpu field yet to be written; every frame
		// before it finished on this thread before the newest began.
		if (index >= m_frames.current_index())
			return false;

		// Older than the ring: the device keeps at most frames_in_flight frames pending and the
		// ring is longer than that, so a frame it has forgotten retired long ago.
		const FrameParams* frame = m_frames.find(index);
		if (frame == nullptr)
			return true;

		// A frame that never reached the GPU carries a zero submission, which reads complete.
		return m_gpu->is_complete(frame->gpu);
	}

	void Runtime::request_quit(int exit_code) noexcept
	{
		m_exit_code.store(exit_code, std::memory_order_relaxed);
		m_quit_requested.store(true, std::memory_order_release);
	}
}
