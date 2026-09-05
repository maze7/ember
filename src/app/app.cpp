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

	/**
	 * Default clear & present, some window managers (Wayland) require a present
	 * before a window is ever rendered. This ensures all users see an empty window.
	 */
	void App::render(const RenderContext& ctx) noexcept
	{
		auto cmd = gpu().begin_command_list();

		cmd.barrier(
			{.texture = ctx.backbuffer,
			 .before  = gpu::TextureState::Undefined,
			 .after	  = gpu::TextureState::RenderTarget});

		gpu::ColorAttachment color{
			.texture = ctx.backbuffer,
			.load	 = gpu::LoadOp::Clear,
			.store	 = gpu::StoreOp::Store,
			.clear =
				{
					.r = 0.02f,
					.g = 0.025f,
					.b = 0.035f,
					.a = 1.0f,
				},
		};

		cmd.begin_rendering({.colors = {&color, 1}});
		cmd.end_rendering();

		cmd.barrier({
			.texture = ctx.backbuffer,
			.before	 = gpu::TextureState::RenderTarget,
			.after	 = gpu::TextureState::Present,
		});

		gpu().submit(cmd);
	}
}
