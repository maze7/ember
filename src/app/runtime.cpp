#include <ember/anim/library.h>
#include <ember/app/runtime.h>
#include <ember/assets/bank_asset.h>
#include <ember/core/logger.h>
#include <ember/core/profile.h>
#include <ember/core/time.h>
#include <ember/gpu/device.h>
#include <ember/imgui/imgui_backend.h>
#include <ember/memory/memory.h>

#include <chrono>
#include <thread>

namespace ember
{
	namespace
	{
		/**
		 * The UI over the frame. The Runtime registers this after the app's own features, so its pass
		 * is the last and lands on top of everything else. It closes the UI frame as it declares its
		 * pass, on the owner thread.
		 */
		class OverlayFeature final : public render::RenderFeature
		{
		public:
			struct Def
			{
			};

			OverlayFeature(render::Renderer&, const Def&) noexcept {}

			void add_passes(render::RenderFrame& frame) noexcept override
			{
				imgui::end_frame(frame.device);

				frame.graph.pass("imgui")
					.color({.texture = frame.resources.output, .load = gpu::LoadOp::Load})
					.record([](gpu::CommandList& cmd) { imgui::render(cmd); });
			}
		};
	}

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

		// The frame's memory, on the shared heap. It is closed between frames: nothing allocates
		// from it before the first frame begins.
		m_scratch.init(memory::tagged_heap(), "frame");

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

		// The UI's context, atlas, cursors and pipeline, which draws into the swapchain's format.
		// Its pass joins the renderer once the app has registered its own features.
		if (config.imgui)
		{
			if (!imgui::init(*m_gpu, *m_platform, {.color_format = m_gpu->swapchain_format(m_swapchain)}))
				return rollback(RuntimeError::ImGuiInitFailed);

			m_imgui = true;
		}

		// A machine that cannot play sound still runs the game: the engine says why, and does nothing.
		m_audio = memory::make_unique<audio::Engine>(MemoryTag::Audio);
		(void)m_audio->init(config.audio);

		m_assets = memory::make_unique<AssetManager>(MemoryTag::Assets);
		m_assets->init(*m_gpu, config.assets);

		// The engine's own types come registered, as the manager's textures and the material library's
		// types do. A game registers its own in its init().
		anim::register_types(*m_assets);
		m_assets->register_type<BankAsset>("bank", m_audio.get());

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

		// After the assets, whose banks unload through it; FMOD's threads stop here, before the heap goes.
		if (m_audio)
		{
			m_audio->shutdown();
			m_audio.reset();
		}

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

			if (m_imgui)
				imgui::shutdown(*m_gpu);

			m_imgui		= false;
			m_ui_extent = {};
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
		// releasing the memory system. The frame's memory goes between the two: the arena frees
		// whatever tag it still holds, and the heap asserts on blocks still owned.
		jobs::shutdown();
		m_scratch.shutdown();
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

		// The app registered all of its features in init(), The ImGui pass should be above everything.
		if (m_imgui)
			m_renderer->add_feature<OverlayFeature>();

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
	 *   sample    input and time
	 *   update()  the game moves on, and what it sounds like goes to the mixer
	 *   render()  draws it
	 *   submit    and the frame's memory goes
	 *
	 * The frame's memory is begun under the frame's tag before update() and freed after the submit,
	 * so whatever either stage builds in it lives exactly as long as the frame.
	 */
	void Runtime::run_frame(App& app) noexcept
	{
		TaggedHeap& heap = memory::tagged_heap();

		// The frame's number is its identity: the ring slot it takes and the sequence half of its
		// memory's tag. Boot was 0, so the first frame is 1.
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
		const f32 dt =
			std::clamp(std::chrono::duration<f32>(tick - m_previous_frame).count(), 0.0f, m_max_delta_seconds);

		m_previous_frame = tick;

		// The frame begins: its slot takes the number, a copy of the input the pump just published,
		// and what it will be drawn on when there is something to draw on.
		FrameParams& frame = m_frames.begin(index, dt, m_input.state());
		m_frame_index	   = index;

		if (!backbuffer.is_null())
		{
			frame.frame_slot		= info.slot;
			frame.backbuffer		= backbuffer;
			frame.backbuffer_extent = m_gpu->swapchain_extent(m_swapchain);
		}

		// The UI frame is open from here until the overlay closes it as the frame draws, so a panel can be drawn from
		// either stage.
		if (m_imgui)
		{
			if (!backbuffer.is_null())
				m_ui_extent = frame.backbuffer_extent;

			imgui::new_frame(m_input.state(), m_window, m_ui_extent, dt);
		}

		m_scratch.begin(heap_tag(MemoryLifetime::Frame, index));

		{
			EMBER_PROFILE_SCOPE_C("update", PROFILE_COLOR_GAMEPLAY);
			frame.update_begin_ns = now_ns();
			app.update(frame);
			frame.update_end_ns = now_ns();
		}

		// Everything the frame asked to be heard goes to the mixer together, with where it is heard from.
		{
			EMBER_PROFILE_SCOPE_C("audio", PROFILE_COLOR_AUDIO);
			m_audio->update();
		}

		if (!backbuffer.is_null())
		{
			// Stamped after the waits for the GPU and the display, so render time is work, not waiting.
			EMBER_PROFILE_SCOPE_C("render", PROFILE_COLOR_RENDER);
			frame.render_begin_ns = now_ns();
			app.render(frame);
			frame.render_end_ns = now_ns();
		}

		// A UI frame that the overlay never closed, either because of a minimized window or a render that
		// never reached the renderer. Always dropped and the next frame opens a fresh one.
		if (m_imgui)
			imgui::discard();

		// The GPU frame closes on every path that opened it. Every copy out of the frame's memory
		// has been recorded by now, so that memory dies here.
		if (drawable)
			(void)m_gpu->end_frame();

		heap.free(m_scratch.end());

		if (m_gpu->device_lost())
		{
			EMBER_ERROR("GPU device lost");
			request_quit(1);
		}
	}

	void Runtime::request_quit(int exit_code) noexcept
	{
		m_exit_code.store(exit_code, std::memory_order_relaxed);
		m_quit_requested.store(true, std::memory_order_release);
	}
}
