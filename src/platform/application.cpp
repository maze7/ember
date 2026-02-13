#include "core/time.h"
#include "platform/application.h"

using namespace Ember;

Application::Application() : m_window("", 1200, 800), m_assets("res") {
	RenderDevice::init(m_window);
	m_batcher = make_unique<Batcher>();
	m_imgui = make_unique<ImGuiRenderer>(m_window, m_input, RenderDevice::instance());
	m_window.set_visible(true);
}

Application::~Application() = default;


void Application::run() {
	EMBER_ASSERT(!is_running());

	if (!init())
		return;

	m_running = true;

	static double accumulator = 0.0;
	static double current_time = 0.0;
	static constexpr double MAX_FRAME_TIME = 0.25; // 250ms

	auto gpu = RenderDevice::instance();

	while (is_running()) {
		// hot-reload any queued assets
		m_assets.update();

		// poll for events and feed to Input
		m_window.set_text_input(m_imgui->wants_text_input());
		m_running = m_window.poll_events(m_input.state());
		gpu->clear({
           	.color = Color::Black,
		    .mask = ClearMask::Color,
		});

		// update timers / delta time
		Time::tick();

		// current frame time (in seconds)
		double new_time = Time::seconds();
		double frame_time = new_time - current_time;
		current_time = new_time;

		if (frame_time > MAX_FRAME_TIME)
			frame_time = MAX_FRAME_TIME;

		accumulator += frame_time;

		// Get dynamic timestep from subclass (allows clock sync adjustment)
		double dt = get_fixed_timestep();

		// Run as many fixed updates as fit in `accumulator`
		while (accumulator >= dt) {
			update_fixed(dt); // update physics, network, etc
			accumulator -= dt;
		}

		update_variable(static_cast<float>(frame_time), accumulator);
		render();

		m_imgui->begin_layout();
		imgui();
		m_imgui->end_layout();
		m_imgui->render();

		// step input for next frame
		m_input.step_state();

		// present the frame to the window
		gpu->present();
	}


	// cleanup
	cleanup();
	m_imgui.reset();
	m_batcher.reset();
	gpu->dispose();
}

void Application::exit() {
	m_running = exit_requested();
}

bool Application::is_running() const {
	return m_running;
}
