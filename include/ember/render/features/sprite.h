#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/command_list.h>
#include <ember/gpu/pipeline.h>
#include <ember/gpu/texture.h>
#include <ember/render/material.h>
#include <ember/render/renderer.h>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace ember::render
{
	/**
	 * The cutout family's material, mirrored in shaders/sprite.slang. A frame is
	 * an atlas plus a uv rect: a unit quad samples the rect, so animation is a
	 * material swap, while a prebaked standee mesh carries final uvs per vertex
	 * and leaves the rect at the full 0..1 identity.
	 */
	struct SpriteMaterial
	{
		u32 albedo		   = 0;
		u32 albedo_sampler = 0;
		f32 alpha_cutoff   = 0.5f;
		u32 pad			   = 0;
		glm::vec2 uv_min   = {0.0f, 0.0f};
		glm::vec2 uv_max   = {1.0f, 1.0f};
		glm::vec4 tint	   = {1.0f, 1.0f, 1.0f, 1.0f};
	};

	static_assert(sizeof(SpriteMaterial) == 48 && std::is_trivially_copyable_v<SpriteMaterial>);

	/// Authoring-space bounds of a unit quad, for objects that draw it: RenderObjectDef::sphere,
	/// scaled by the objects transform.
	inline constexpr glm::vec4 SPRITE_QUAD_SPHERE = {0.5f, 0.5f, 0.0f, 0.7072f};

	/**
	 * The cutout standee family: alpha-tested quads that share the scene's
	 * geometry, culling and depth. It draws from the cutout visibility bucket
	 * after the opaque world, so its fragments test against the world depth and
	 * overlapping standees sort by their foot depth. Double sided and depth
	 * writing, with discard shaping the silhouette instead of blending, so the
	 * draws stay order independent. This is how grass, props and characters stand
	 * in the 3D world while reading as flat pixel art.
	 *
	 * Register after the world family that publishes scene_color and scene_depth,
	 * and before the upscale. Owns its material pool like every family feature.
	 */
	class SpriteFeature final : public RenderFeature
	{
	public:
		struct Def
		{
			/// Cooked SPIR-V shader, uses embedded shaders/sprite.slang if empty.
			Span<const u8> shader = {};

			/// Match the world family the sprites compose onto.
			gpu::TextureFormat color_format = gpu::TextureFormat::RGBA16Float;
			gpu::TextureFormat depth_format = gpu::TextureFormat::D32Float;

			u32 material_capacity = 256;
		};

		SpriteFeature(Renderer& renderer, const Def& def) noexcept;

		void shutdown(gpu::Device& device) noexcept override;
		void add_passes(RenderFrame& frame) noexcept override;

		/// The family's material table; the game creates and edits through it.
		[[nodiscard]] MaterialPool& materials() noexcept { return m_materials; }

		/// The unit card every sprite can draw: [0,1] x [0,1], uv v0 at the top,
		/// up normal. Games with exotic cards make their own.
		[[nodiscard]] GeometryHandle quad() const noexcept { return m_quad; }

	private:
		Renderer* m_renderer = nullptr; // owner; outlives the feature

		GraphicsPipelineHandle m_pipeline = {};
		GeometryHandle m_quad			  = {};
		MaterialPool m_materials;
	};
}
