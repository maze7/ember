#pragma once

#include <ember/gpu/common.h>
#include <ember/app/app.h>
#include <ember/core/common.h>
#include <ember/core/result.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/unique.h>
#include <ember/platform/platform.h>

#include <chrono>

namespace ember
{
	enum class RuntimeError : u8
	{
		AlreadyInit,
		InvalidConfig,
		MemoryInitFailed,
		PlatformInitFailed,
		DeviceInitFailed,
		WindowInitFailed,
		SwapchainInitFailed,
	};

	/**
	 * Owns the engine services shared by one application instance.
	 *
	 * init() constucts services in dependency order. shutdown() releases them
	 * in reverse order and is safe after partial initialization.
	 */
	class Runtime final
	{
	public:
		Runtime() noexcept = default;
		~Runtime() noexcept;

		// Runtime is the main engine orchestrator, it should not be copied or moved.
		Runtime(const Runtime&)			   = delete;
		Runtime& operator=(const Runtime&) = delete;
		Runtime(Runtime&&)				   = delete;
		Runtime& operator=(Runtime&&)	   = delete;

		[[nodiscard]] Result<void, RuntimeError> init(const AppConfig& config, const Args& args) noexcept;
		void shutdown() noexcept;

		[[nodiscard]] bool initialized() const noexcept;

		int run(App& app) noexcept;

	private:
		friend class App;

		enum class State : u8
		{
			Empty,
			Initializing,
			Ready,
			Running,
		};

		void request_quit(int exit_code) noexcept;
		void frame_loop(App& app) noexcept;

		Unique<MemorySystem> m_memory;
		Unique<jobs::JobSystem> m_jobs;
		Unique<Platform> m_platform;
		Unique<gpu::Device> m_gpu;
		Unique<render::Renderer> m_renderer;

		Input m_input;

		State m_state				= State::Empty;
		Args m_args					= {};
		WindowHandle m_window		= {};
		SwapchainHandle m_swapchain = {};

		std::chrono::steady_clock::time_point m_previous_frame = {};

		f32 m_max_delta_seconds = 0.1f;
		u64 m_frame_index		= 0;
		int m_exit_code			= 0;
		bool m_quit_requested	= false;
	};
}
