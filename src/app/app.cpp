#include <ember/app/app.h>
#include <ember/app/runtime.h>
#include <ember/core/common.h>

namespace ember
{
	const Args& App::args() const noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return m_runtime->args();
	}

	Platform& App::platform() noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return m_runtime->m_platform;
	}

	gpu::Device& App::gpu() noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return m_runtime->m_gpu;
	}

	const Input& App::input() const noexcept
	{
		EMBER_ASSERT(m_runtime != nullptr);
		return m_runtime->m_input;
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
}
