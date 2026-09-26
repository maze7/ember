#include <ember/app/app.h>
#include <ember/app/runtime.h>
#include <ember/core/common.h>

namespace ember
{
	const Args& App::args() const noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return m_runtime->m_args;
	}

	Platform& App::platform() noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		EMBER_ASSERT(jobs::is_main() && "the platform is the owner thread's; update() runs as a job");
		return *m_runtime->m_platform;
	}

	gpu::Device& App::gpu() noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		EMBER_ASSERT(jobs::is_main() && "the device is the owner thread's; update() runs as a job");
		return *m_runtime->m_gpu;
	}

	render::Renderer& App::renderer() noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return *m_runtime->m_renderer;
	}

	WindowHandle App::window() const noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return m_runtime->m_window;
	}

	SwapchainHandle App::swapchain() const noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return m_runtime->m_swapchain;
	}

	void App::quit(int exit_code) noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		m_runtime->request_quit(exit_code);
	}

	const FrameParams* App::frame(u64 index) const noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return m_runtime->m_frames.find(index);
	}

	AssetManager& App::assets() noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return *m_runtime->m_assets;
	}

	bool App::is_frame_complete(u64 index) const noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return m_runtime->is_frame_complete(index);
	}
}
