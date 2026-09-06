#pragma once

#include <ember/platform/platform.h>
#include <ember/app/app.h>

#include <chrono>

namespace ember
{
	/**
	 * Owns the engine services shared by one application instance.
	 *
	 * Runtime is constructed before before App and destroyed after App. That ordering
	 * keeps the allocator, platform, and GPU live during application teardown.
	 */
	class Runtime final
	{
	public:
		Runtime(const AppConfig& config, const Args& args) noexcept;
		~Runtime() noexcept;

		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;
		Runtime(Runtime&&) = delete;
		Runtime& operator=(Runtime&&) = delete;

		[[nodiscard]] explicit operator bool() const noexcept
        {
            return m_valid;
        }

		int run(App& app) noexcept;

		const Args& args() const noexcept
		{
			return m_args;
		}

		Platform& platform() noexcept
		{
			return m_platform;
		}

	private:
		friend class App;

		class FrameScope final
        {
        public:
            explicit FrameScope(Runtime& runtime) noexcept;
            ~FrameScope() noexcept;

            FrameScope(const FrameScope&) = delete;
            FrameScope& operator=(const FrameScope&) = delete;
            FrameScope(FrameScope&&) = delete;
            FrameScope& operator=(FrameScope&&) = delete;

            [[nodiscard]] const gpu::FrameInfo& info() const noexcept
            {
                return m_info;
            }

        private:
            Runtime& m_runtime;
            gpu::FrameInfo m_info = {};
        };

        void request_quit(int exit_code) noexcept;
        void close_frame() noexcept;

		// Declaration order is initialization order.
		MemorySystem m_memory;
		Platform m_platform;
		gpu::Device m_gpu;
		Input m_input;
		render::Renderer m_renderer;

		Args m_args;

		WindowHandle m_window = {};
		SwapchainHandle m_swapchain = {};

		std::chrono::steady_clock::time_point m_previous_frame = {};

		f32 m_max_delta_seconds = 0.1f;
		u64 m_frame_index = 0;

		int m_exit_code = 0;

		bool m_frame_open = false;
        bool m_quit_requested = false;
        bool m_has_run = false;
        bool m_valid = false;
	};
}
