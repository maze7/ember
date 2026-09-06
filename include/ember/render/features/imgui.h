#pragma once

#include <ember/imgui/imgui_backend.h>
#include <ember/render/renderer.h>

namespace ember::imgui
{
	/**
	 * Draws the ImGui frame over the renderer output; register last so the
	 * overlay lands on top. Lives in the imgui module so the render module
	 * never links the optional dependency. Header only: the game's link of
	 * both modules provides every symbol.
	 */
	class OverlayFeature final : public render::RenderFeature
	{
	public:
		struct Def
		{
		};

		OverlayFeature(render::Renderer& render, const Def&) noexcept {}

		void add_passes(render::RenderFrame& frame) noexcept override
		{
			frame.graph.pass("imgui")
				.color({.texture = frame.resources.output, .load = gpu::LoadOp::Load})
				.record([](gpu::CommandList& cmd) { render(cmd); });
		}
	};
}
