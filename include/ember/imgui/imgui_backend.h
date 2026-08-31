#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/gpu/texture.h>
#include <ember/platform/window.h>

#include <imgui.h>

namespace ember
{
	class Input;
	class Platform;
}

namespace ember::gpu
{
	class Device;
	class CommandList;
}

namespace ember::imgui
{
	struct BackendDef
	{
		/// Cooked imgui.slang blob, entries vs_main / fs_main
		Span<const u8> shader = {};

		// Format of the target the UI pass draws into.
		gpu::TextureFormat color_format = gpu::TextureFormat::Undefined;
	};

	/**
	 * Creates the ImGui context, font atlas, pipeline and cursors.
	 * Device and platform must outlive shutdown().
	 */
	[[nodiscard]] bool init(gpu::Device& device, Platform& platform, const BackendDef& def) noexcept;
	void shutdown(gpu::Device& device) noexcept;

	/**
	 * Feeds input and opens a UI frame. Once per update, after pump_events and
	 * before any ImGui:: calls. Owns cursor shape and text input activation
	 * while the UI wants them.
	 */
	void new_frame(const Input& input, WindowHandle window, Extent2D display, f32 dt) noexcept;

	/// Records the frame's draw data. Call inside an open rendering pass whose
	/// target matches BackendDef::color_format. Leaves the scissor modified.
	void render(gpu::CommandList& cmd) noexcept;

	/// Ends a UI frame without drawing it (minimized window, skipped frame).
	void discard() noexcept;

	/// True while the UI wants the device; game input should skip it.
	[[nodiscard]] bool wants_mouse() noexcept;
	[[nodiscard]] bool wants_keyboard() noexcept;

	/// Any engine texture as an ImGui image. A destroyed handle shows the heap fallback.
	[[nodiscard]] inline ImTextureID texture_id(TextureHandle texture) noexcept
	{
		return static_cast<ImTextureID>(bindless_index(texture));
	}
}
