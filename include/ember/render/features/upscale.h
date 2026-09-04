#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/pipeline.h>
#include <ember/gpu/sampler.h>
#include <ember/gpu/texture.h>
#include <ember/render/renderer.h>

#include <glm/vec2.hpp>

namespace ember::render
{
	/**
	 * The scene renders into an internal HDR target at a low, art-driven resolution,
	 * and this feature upscales it to the output with sharp bilinear filtering (nearest
	 * inside a texel, one output pixel of blend at texel edges, pure nearest at integer
	 * scales).
	 *
	 * prepare() publishes scene_color and scene_extent, so world features render
	 * internal without knowing this feature exists; without it they draw straight to
	 * the output. Register after the world features and before native resolution overlays.
	 *
	 * The subtexel offset closes the camera snapping loop: a snapped ortho
	 * view keeps geometry texel locked and hands its remainder here, so
	 * panning moves the sampling window instead of shimmering the pixels.
	 */
	class UpscaleFeature final : public RenderFeature
	{
	public:
		struct Def
		{
			/// Cooked shaders/upscale.slang.
			Span<const u8> shader = {};

			/// Must match the output the renderer draws into.
			gpu::TextureFormat output_format = gpu::TextureFormat::RGBA8Unorm;

			Extent2D resolution = {640, 360};

			/// Texel remainder from a snapped camera; null means zero.
			const glm::vec2* subtexel_offset = nullptr;
		};

		UpscaleFeature(gpu::Device& device, const Def& def) noexcept;

		void shutdown(gpu::Device& device) noexcept override;
		void prepare(RenderFrame& frame) noexcept override;
		void add_passes(RenderFrame& frame) noexcept override;

		/// Live knob for the on-screen resolution comparison.
		void set_resolution(Extent2D resolution) noexcept { m_resolution = resolution; }
		[[nodiscard]] Extent2D resolution() const noexcept { return m_resolution; }

	private:
		GraphicsPipelineHandle m_pipeline = {};
		SamplerHandle m_sampler			  = {};

		Extent2D m_resolution			   = {640, 360};
		const glm::vec2* m_subtexel_offset = nullptr;
	};
}
