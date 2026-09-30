#pragma once

#include <ember/core/common.h>
#include <ember/gpu/command_list.h>
#include <ember/gpu/common.h>
#include <ember/render/view.h>

#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <type_traits>

namespace ember::render
{
	/**
	 * The scene's light for the frame, mirrored in shaders/frame.slang: what a lighting feature
	 * publishes in prepare(). The defaults are what surfaces see without one, a white sky and ground
	 * and no lights, under which the lit models shade (close to) as authored.
	 */
	struct LightingConstants
	{
		glm::vec3 sky	= {1.0f, 1.0f, 1.0f}; // linear
		u32 light_count = 0;

		glm::vec3 ground = {1.0f, 1.0f, 1.0f};
		u32 lights		 = 0; // the LightData table

		u32 first_light = 0; // the frame's first record in it
		u32 bands		= 0; // Pixel model: steps per light, zero for smooth
		f32 dither		= 0.0f;
		f32 dither_cell = 1.0f; // world units

		u32 shadows		 = 0; // the ShadowData table
		u32 first_shadow = 0; // the frame's first record in it
		u32 pad0		 = 0;
		u32 pad1		 = 0;
	};

	static_assert(sizeof(LightingConstants) == 64 && std::is_trivially_copyable_v<LightingConstants>);
	static_assert(offsetof(LightingConstants, ground) == 16 && offsetof(LightingConstants, first_light) == 32 &&
				  offsetof(LightingConstants, shadows) == 48);

	/**
	 * What every scene pass reads first, mirrored in shaders/frame.slang and bound at CONSTANTS_FRAME:
	 * the frame's time, and the scene's tables by bindless slot, so no pass carries them in constants
	 * of its own. Constant blocks lay out by std140, so members group into 16 byte rows by hand and
	 * the asserts pin every row.
	 */
	struct FrameConstants
	{
		f32 time		= 0.0f; // seconds, wrapped every hour so an f32 stays sub-millisecond precise
		f32 delta_time	= 0.0f;
		u32 frame_index = 0;
		u32 flags		= 0; // engine features on this frame: shaders branch on them, never permute

		u32 objects		  = 0;
		u32 transforms	  = 0;
		u32 instances	  = 0;
		u32 material_keys = 0;

		u32 positions  = 0;
		u32 attributes = 0;
		u32 geometries = 0;
		u32 pad0	   = 0;

		LightingConstants lighting = {}; // the frame's, not the view's: every view sees the same lights
	};

	static_assert(sizeof(FrameConstants) == 112 && std::is_trivially_copyable_v<FrameConstants>);
	static_assert(offsetof(FrameConstants, objects) == 16 && offsetof(FrameConstants, positions) == 32 &&
				  offsetof(FrameConstants, lighting) == 48);

	/// The view a pass renders, mirrored in shaders/frame.slang and bound at CONSTANTS_PASS.
	struct ViewConstants
	{
		glm::mat4 view_projection		  = glm::mat4(1.0f);
		glm::mat4 view					  = glm::mat4(1.0f);
		glm::mat4 inverse_view_projection = glm::mat4(1.0f);

		/// The eye in homogeneous coordinates: (position, 1) for a perspective view and (the
		/// direction toward the camera, 0) for an orthographic one, so shaders take the view
		/// direction as normalize(eye.xyz - world * eye.w) for both, without a branch.
		glm::vec4 eye = {0.0f, 0.0f, 0.0f, 1.0f};

		glm::vec2 resolution		 = {};
		glm::vec2 inverse_resolution = {};

		u32 scene_color	  = 0; // the opaque world's colour, depth and sampler: for transparent and
		u32 scene_depth	  = 0; // screen passes, once a feature copies them out
		u32 scene_sampler = 0;
		u32 pad0		  = 0;
	};

	static_assert(sizeof(ViewConstants) == 240 && std::is_trivially_copyable_v<ViewConstants>);
	static_assert(offsetof(ViewConstants, eye) == 192 && offsetof(ViewConstants, resolution) == 208 &&
				  offsetof(ViewConstants, scene_color) == 224);

	/// A view's matrices, eye and resolution. Its lighting and the scene targets are the frame's, for
	/// the renderer to fill.
	[[nodiscard]] inline ViewConstants view_constants(const View& view) noexcept
	{
		ViewConstants constants;
		constants.view_projection		  = view.view_projection;
		constants.view					  = view.view;
		constants.inverse_view_projection = glm::inverse(view.view_projection);

		// An orthographic projection's w row ignores z; a perspective one's copies it.
		const bool orthographic = view.projection[2][3] == 0.0f;
		constants.eye = orthographic ? glm::vec4(-view_forward(view.view), 0.0f) : glm::vec4(view.position, 1.0f);

		const glm::vec2 resolution{static_cast<f32>(view.extent.width), static_cast<f32>(view.extent.height)};
		constants.resolution		 = resolution;
		constants.inverse_resolution = {resolution.x > 0.0f ? 1.0f / resolution.x : 0.0f,
										resolution.y > 0.0f ? 1.0f / resolution.y : 0.0f};
		return constants;
	}

	/// Binds both blocks, as a scene pass does once, before its first draw.
	inline void bind_scene_constants(gpu::CommandList& cmd, const FrameConstants& frame,
									 const ViewConstants& view) noexcept
	{
		cmd.set_constants(CONSTANTS_FRAME, frame);
		cmd.set_constants(CONSTANTS_PASS, view);
	}
}
