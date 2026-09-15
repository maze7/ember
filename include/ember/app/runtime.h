#pragma once

#include <ember/app/app.h>
#include <ember/core/common.h>
#include <ember/core/result.h>
#include <ember/gpu/common.h>
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
	 * Owns the engine services used by an application session.
	 *
	 * Runtime becomes thread-affine after init() because the platform and GPU backends
	 * must be driven and destroyed by their owner thread. Only one Runtime may be initialized
	 * in a process at a time because several owned services publish process-wide state.
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

		/**
		 * Initializes the Runtime on the calling thread.
		 *
		 * Runtime borrows `args`; the argument array and its strings must remain valid
		 * until shutdown(). Calling init() on a non-empty Runtime returns
		 * Runtime::AlreadyInitialized without disturbing the active state.
		 */
		[[nodiscard]] Result<void, RuntimeError> init(const AppConfig& config, const Args& args) noexcept;

		/**
		 * Releases the services owned by the Runtime.
		 *
		 * Call only after run() has returned and every job that can access Runtime
		 * owned state has completed.
		 */
		void shutdown() noexcept;

		/** Returns true if the Runtime has initialized successfully. */
		[[nodiscard]] bool initialized() const noexcept;

		/**
		 * Runs `app` synchronously until the platform or application requests exit.
		 *
		 * Lifecycle callbacks execute on the Runtime's owner thread under the job
		 * scheduler. This allows jobs::wait() to park the main fiber without moving
		 * thread-affine platform or GPU work to another thread.
		 *
		 * The App remains bound throughout its callbacks, and App::shutdown() is called
		 * after every App::init() attempt, including failed initialization. Returns
		 * the code requested through App::quit(), or one when the App cannot be run
		 * successfully.
		 */
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
