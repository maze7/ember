#pragma once

#include <ember/core/common.h>
#include <ember/render/frame_constants.h>
#include <ember/render/renderer.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <type_traits>

namespace ember::render
{
	/// One shadow map as the shaders read it, mirrored in shaders/shading.slang.
	struct ShadowData
	{
		glm::mat4 world_to_map = glm::mat4(1.0f); // world to the map's (u, v, depth), depth biased toward the light
		u32 map				   = 0;				  // the depth texture's bindless index
		f32 size			   = 0.0f;			  // texels across
		u32 pad[2]			   = {};
	};

	static_assert(sizeof(ShadowData) == 80 && std::is_trivially_copyable_v<ShadowData>);

	/**
	 * How a directional light's shadow map covers the main view. The map is fit each frame to the part
	 * of the view between two heights, the ground and the tops of the tallest casters, because that is
	 * where every shadow falls: a pixel-art camera sees a long, thin box, and fitting the whole box
	 * would spend the map on empty air.
	 */
	struct ShadowFit
	{
		u32 resolution = 2048;	  // texels across, square
		f32 min_height = 0.0f;	  // world y: the lowest a shadow can fall
		f32 max_height = 64.0f;	  // and the highest
		f32 distance   = 1024.0f; // a perspective view's shadows end this far from its near plane
	};

	/**
	 * A directional light's shadow view: orthographic, down the light, covering what `camera` sees
	 * between the fit's heights. Its texels are fixed in the world. The map keeps its size while the
	 * camera pans and its origin moves in whole texels, so shadow edges hold still instead of crawling.
	 * It draws casters from every layer, and has no near plane: a caster between the light and the
	 * receivers still lands in the map, clamped to its nearest depth. False when the camera sees
	 * nothing between the heights.
	 */
	[[nodiscard]] bool make_shadow_view(const View& camera, glm::vec3 direction, const ShadowFit& fit,
										View& out) noexcept;

	/// The matrix a receiver uses: world to the map's (u, v, depth), with half a texel of depth
	/// toward the light so a lit surface compared against its own depth stays lit.
	[[nodiscard]] glm::mat4 shadow_map_matrix(const View& shadow, u32 resolution) noexcept;

	/**
	 * The scene's light, published for every pass that shades: the scene's lights as a table, the
	 * hemisphere ambient, the Pixel model's steps, and the sun's shadow.
	 *
	 * Each frame, prepare() copies every live light into the transient ring and points the frame's
	 * constants at the copy. Copying them all is the whole update scheme: a light is 64 bytes, so a
	 * thousand are 64 KB of sequential writes, with no dirty tracking to get wrong and no GPU buffer
	 * to grow or fence. Every surface loops over every light until the clustering arrives.
	 *
	 * The first directional light that asks for a shadow gets the shadow map: prepare() fits a view
	 * down the light to the main view, adds it to the frame and publishes the map in FrameResources,
	 * and the surface feature draws every caster into it. Deciding in prepare() is what lets the light
	 * table name its map in the same pass that writes it. The map is created the first time a light
	 * asks, so a game without shadows never pays for it.
	 *
	 * Its defaults are what surfaces see without it, a white sky and ground and no lights, so
	 * registering it changes nothing until the game adds a light or sets the ambient. Register it
	 * before the features that shade.
	 */
	class LightingFeature final : public RenderFeature
	{
	public:
		struct Def
		{
			/// The hemisphere ambient in sRGB, as a colour picker gives it: the light arriving from
			/// straight up, and from straight down.
			glm::vec3 sky	 = {1.0f, 1.0f, 1.0f};
			glm::vec3 ground = {1.0f, 1.0f, 1.0f};

			/// Pixel model: steps per light, zero for smooth, and how much of each step is a dithered
			/// blend into the next, from 0 for hard edges to 1 for a stipple the whole way across.
			/// Smooth is the default because the looks this engine aims at step their light through
			/// the art's pixels, not its values. A level that holds still between two steps, a sunlit
			/// floor, dithers into a halftone across all of it, so dither suits light that varies.
			u32 bands  = 0;
			f32 dither = 0.0f;

			/// How much world a dither cell covers. Cells are laid out in the view's plane and
			/// anchored to the world, so at the art's texel size the stipple sits on its pixels
			/// whatever the screen scale, and holds still as the camera moves.
			f32 dither_cell = 1.0f;

			ShadowFit shadow = {};
		};

		LightingFeature(Renderer& renderer, const Def& def) noexcept;

		void shutdown(gpu::Device& device) noexcept override;
		void prepare(RenderFrame& frame) noexcept override;
		void add_passes(RenderFrame&) noexcept override {}

		/// Runtime knobs: time of day, a cave's gloom, an editor's sliders. Colours in sRGB.
		void set_ambient(glm::vec3 sky, glm::vec3 ground) noexcept;
		void set_bands(u32 bands, f32 dither) noexcept;

	private:
		/// Fits, adds and publishes the shadow view for a light travelling along `direction`, and
		/// points the frame's constants at its table. False, with nothing published, when the main
		/// view sees no ground to shadow or the frame has no room.
		[[nodiscard]] bool shadow(RenderFrame& frame, glm::vec3 direction) noexcept;

		/// All but the tables, which prepare() fills in each frame.
		LightingConstants m_constants = {};

		ShadowFit m_fit			   = {};
		TextureHandle m_shadow_map = {}; // made when a light first asks for a shadow
	};
}
