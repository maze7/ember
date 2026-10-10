#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/pipeline.h>
#include <ember/render/renderer.h>

#include <glm/vec3.hpp>

namespace ember::render
{
	/// Screen materials a frame draws, in the order the game lists them.
	inline constexpr u32 MAX_SCREEN_MATERIALS = 8;

	/// Bloom levels, each half the one above: eight take a 4K scene down to 15 texels.
	inline constexpr u32 MAX_BLOOM_LEVELS = 8;

	/**
	 * A colour grade as a lookup table: what every display colour becomes, sampled on a size³ grid
	 * and laid out as the strip grading tools export, `size` slices of size x size side by side, blue
	 * choosing the slice, red across it and green down it. An sRGB texture of size² x size texels, as a
	 * PNG loads: a strip loaded as a TextureAsset is {texture, extent.height}. 32 is the usual size,
	 * fine enough that gradients do not band.
	 */
	struct ColorLut
	{
		TextureHandle texture = {};
		u32 size			  = 0; // cells along each axis; under 2 is no table
	};

	/**
	 * A grade made in code. `grade` maps a display colour (sRGB, each channel 0..1) to the colour it
	 * becomes, and runs once per cell; the identity makes the neutral strip. Made on the owner thread,
	 * it uploads ahead of the frame's work and is right on the first frame that reads it. The caller
	 * destroys the texture.
	 */
	[[nodiscard]] ColorLut create_color_lut(gpu::Device& device, u32 size, glm::vec3 (*grade)(glm::vec3)) noexcept;

	/// The same, for a grade that reads its settings from somewhere: `context` comes back with every cell.
	[[nodiscard]] ColorLut create_color_lut(gpu::Device& device, u32 size, glm::vec3 (*grade)(glm::vec3, const void*),
											const void* context) noexcept;

	/**
	 * The scene as it is shown: bloom, a grade, then the game's screen materials, on the HDR scene
	 * colour before the upscale enlarges it. Everything runs at the scene's resolution, so a glow and a
	 * graded pixel land on the art's grid like the rest of it.
	 *
	 * Bloom spreads the light over a threshold: a chain of half-size levels filtered down with
	 * Jimenez's thirteen taps, then summed back up with a tent, a narrow core with a long soft tail.
	 * The grade adds it, applies exposure and a shoulder that rolls highlights off above a knee, then
	 * looks the colour up in two tables and blends between them, so time of day or a change of place
	 * moves the look without an artist painting every step in between.
	 *
	 * Screen materials follow, in the game's order, each reading the image so far through
	 * scene_color() and the world's depth through scene_depth(). A material that returns the whole
	 * image draws into a second target and the two trade places; one that blends draws over the image
	 * in place, since no pass can read the target it draws on.
	 *
	 * The defaults change nothing: no bloom, no tables, exposure 1 and a knee of 1, which only clips
	 * to what the display shows anyway. It needs an intermediate scene colour, the upscale's, and does
	 * nothing without one. Register it after the world's features and before the upscale.
	 */
	class ScreenFeature final : public RenderFeature
	{
	public:
		struct Bloom
		{
			/// How much of the light over the threshold spreads into the glow. Zero is off, and skips
			/// the chain.
			f32 intensity = 0.0f;

			/// Linear. At 1 only what the display cannot show blooms: a lamp, a spell, a white wall
			/// under a strong sun, never the art's own colours under ordinary light.
			f32 threshold = 1.0f;

			/// The glow's reach: each level doubles it.
			u32 levels = 6;
		};

		struct Def
		{
			/// Cooked SPIR-V override; empty uses the engine's embedded shaders/post.slang.
			Span<const u8> shader = {};

			Bloom bloom = {};

			/// Scales the scene before the shoulder: the camera's exposure, in linear units.
			f32 exposure = 1.0f;

			/// Where the shoulder starts rolling highlights off toward white; 1 clips.
			f32 knee = 1.0f;
		};

		ScreenFeature(Renderer& renderer, const Def& def) noexcept;

		void shutdown(gpu::Device& device) noexcept override;
		void prepare(RenderFrame& frame) noexcept override;
		void add_passes(RenderFrame& frame) noexcept override;

		/// Runtime knobs: a torch's glow, a cave's exposure, an editor's sliders.
		void set_bloom(const Bloom& bloom) noexcept;
		void set_tone(f32 exposure, f32 knee) noexcept;

		/// The grade: `blend` runs from `from` at 0 to `to` at 1. A table with no texture is the
		/// identity, so one table fades in from the ungraded scene. Tables must be resident: a streamed
		/// texture reads the heap's fallback until its pixels land.
		void set_grade(ColorLut from, ColorLut to, f32 blend) noexcept;

		/// The screen materials to draw this frame and after, in order. A material that dies, or whose
		/// type is not a screen type, is skipped rather than drawn wrong.
		void set_materials(Span<const MaterialHandle> materials) noexcept;

	private:
		/// A screen type's pipeline, and the build of the type it was made from.
		struct TypePipeline
		{
			MaterialTypeHandle type			= {};
			u32 generation					= 0;
			GraphicsPipelineHandle pipeline = {};
			bool blends						= false; // draws over the image instead of replacing it
		};

		/// The bloom chain's top level, which holds every level's light once the chain is summed.
		struct BloomChain
		{
			GraphTexture top = {};
			Extent2D extent	 = {};
			u32 levels		 = 0;
		};

		[[nodiscard]] BloomChain add_bloom(RenderFrame& frame) const noexcept;
		[[nodiscard]] GraphTexture add_grade(RenderFrame& frame, const BloomChain& bloom) const noexcept;
		[[nodiscard]] GraphTexture add_materials(RenderFrame& frame, GraphTexture image) const noexcept;

		static void destroy(gpu::Device& device, TypePipeline& entry) noexcept;

		Vector<TypePipeline> m_pipelines; // one per type slot, filled for the screen types

		GraphicsPipelineHandle m_down  = {};
		GraphicsPipelineHandle m_up	   = {};
		GraphicsPipelineHandle m_grade = {};
		SamplerHandle m_sampler		   = {};

		Bloom m_bloom	= {};
		f32 m_exposure	= 1.0f;
		f32 m_knee		= 1.0f;
		ColorLut m_from = {};
		ColorLut m_to	= {};
		f32 m_blend		= 0.0f;

		MaterialHandle m_materials[MAX_SCREEN_MATERIALS] = {};
		u32 m_material_count							 = 0;
	};
}
