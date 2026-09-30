#pragma once

#include <ember/core/common.h>
#include <ember/gpu/pipeline.h>
#include <ember/gpu/texture.h>
#include <ember/render/renderer.h>

namespace ember::render
{
	/**
	 * Draws every surface material type on scene geometry: the one feature a game registers for its
	 * world, whatever mix of materials it uses.
	 *
	 * Each type is a draw bucket with one pipeline, built from the type's bytecode and state plus
	 * what only this feature knows: its targets and the depth test. Pipelines are built in
	 * prepare(), so a type pays for its pipeline once, on the first frame it exists, never in the
	 * middle of a pass. A type the registry replaces (hot reload) gets its pipeline rebuilt at the
	 * next frame, and a removed type's is released.
	 *
	 * Each frame draws the main view's buckets in two passes, one indirect multi-draw per bucket:
	 * opaque types, then cutout types, both writing depth, so cutout sprites sort against the world
	 * and each other by depth; then transparent types over them, depth writes off. Inside a queue,
	 * [Priority] orders the types. The CPU cost is buckets times passes, whatever the object count.
	 *
	 * It owns scene_depth and draws into scene_color when a feature publishes one (the upscale),
	 * else straight into the output. Register it before the upscale and the overlays.
	 */
	class SurfaceFeature final : public RenderFeature
	{
	public:
		struct Def
		{
			/// Match the target: the swapchain format standalone, RGBA16Float under the upscale.
			gpu::TextureFormat color_format = gpu::TextureFormat::RGBA8Unorm;
			gpu::TextureFormat depth_format = gpu::TextureFormat::D32Float;

			gpu::ClearColor clear = {0.0f, 0.0f, 0.0f, 1.0f};

			/// How far casters are pushed from the light as they are drawn into a shadow map, so a
			/// lit surface does not shadow itself where the map's texels cut across it. Slope-scaled:
			/// two texels' worth of the caster's own depth change, the most a snapped lookup can miss
			/// by, and nothing for a surface facing the light. Negative is away under reverse Z.
			gpu::DepthBias shadow_bias = {.slope = -2.0f};
		};

		SurfaceFeature(Renderer& renderer, const Def& def) noexcept;

		void shutdown(gpu::Device& device) noexcept override;
		void prepare(RenderFrame& frame) noexcept override;
		void add_passes(RenderFrame& frame) noexcept override;

	private:
		/// A bucket's pipeline, and the build of its type it was made from.
		struct BucketPipeline
		{
			MaterialTypeHandle type			= {};
			u32 generation					= 0;
			GraphicsPipelineHandle pipeline = {};
			GraphicsPipelineHandle shadow	= {}; // depth only, for types that cast shadows
		};

		[[nodiscard]] GraphicsPipelineHandle build(gpu::Device& device, const material::Type& type) const noexcept;

		[[nodiscard]] GraphicsPipelineHandle build_shadow(gpu::Device& device,
														  const material::Type& type) const noexcept;

		static void destroy(gpu::Device& device, BucketPipeline& entry) noexcept;

		Vector<BucketPipeline> m_pipelines; // one per bucket

		gpu::TextureFormat m_color_format = gpu::TextureFormat::RGBA8Unorm;
		gpu::TextureFormat m_depth_format = gpu::TextureFormat::D32Float;
		gpu::ClearColor m_clear			  = {};
		gpu::DepthBias m_shadow_bias	  = {};
	};
}
