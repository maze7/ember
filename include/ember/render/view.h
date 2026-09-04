#pragma once

#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/render/common.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cmath>

namespace ember::render
{
	/**
	 * Six world-space planes, inward facing: a point is inside when
	 * dot(plane.xyz, point) + plane.w >= 0. Planes with a normal are normalized
	 * so the comparison works in world units against a sphere radius; an
	 * infinite far projection yields a far plane with no normal and positive w,
	 * which every sphere passes. That is the meaning of far at infinity, and it
	 * keeps finite far views (shadow orthos) on the same code path.
	 */
	struct Frustum
	{
		glm::vec4 planes[6] = {};
	};

	struct View
	{
		glm::mat4 view			  = glm::mat4(1.0f);
		glm::mat4 projection	  = glm::mat4(1.0f);
		glm::mat4 view_projection = glm::mat4(1.0f);

		Frustum frustum = {};

		glm::vec3 position = {};
		Extent2D extent	   = {};

		LayerMask layers = LAYER_ALL;
		const char* name = "view";
	};

	/// Gribb Hartmann extraction in Vulkan clip conventions: x and y in [-w, w],
	/// z in [0, w]. Works for any projection, perspective or ortho, either depth
	/// direction; only the plane names swap under reverse Z.
	[[nodiscard]] inline Frustum frustum_from(const glm::mat4& view_projection) noexcept
	{
		// Rows of the matrix in math convention, gathered from glm's columns.
		const glm::vec4 row0{
			view_projection[0][0], view_projection[1][0], view_projection[2][0], view_projection[3][0]};
		const glm::vec4 row1{
			view_projection[0][1], view_projection[1][1], view_projection[2][1], view_projection[3][1]};
		const glm::vec4 row2{
			view_projection[0][2], view_projection[1][2], view_projection[2][2], view_projection[3][2]};
		const glm::vec4 row3{
			view_projection[0][3], view_projection[1][3], view_projection[2][3], view_projection[3][3]};

		Frustum frustum{
			.planes = {
				row3 + row0, // x >= -w
				row3 - row0, // x <= w
				row3 + row1, // y >= -w
				row3 - row1, // y <= w
				row2,		 // z >= 0, the far plane under reverse Z
				row3 - row2, // z <= w, the near plane under reverse Z
			}};

		for (glm::vec4& plane : frustum.planes)
		{
			const f32 length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
			if (length > 1e-6f)
				plane /= length;
		}

		return frustum;
	}

	/// Conservative sphere test: exact against each plane, so distant corner
	/// cases pass as visible rather than vanish. The cull kernel mirrors this.
	[[nodiscard]] inline bool intersects(const Frustum& frustum, glm::vec4 sphere) noexcept
	{
		for (const glm::vec4& plane : frustum.planes)
		{
			if (glm::dot(glm::vec3{plane}, glm::vec3{sphere}) + plane.w < -sphere.w)
				return false;
		}

		return true;
	}

	/// Camera world position recovered from a view matrix.
	[[nodiscard]] inline glm::vec3 view_position(const glm::mat4& view) noexcept
	{
		// eye = -transpose(R) * t; vec * mat is the transpose product in glm.
		const glm::mat3 rotation{view};
		return -(glm::vec3{view[3]} * rotation);
	}

	/// Camera world forward recovered from a view matrix.
	[[nodiscard]] inline glm::vec3 view_forward(const glm::mat4& view) noexcept
	{
		return -glm::vec3{view[0][2], view[1][2], view[2][2]};
	}

	[[nodiscard]] inline View make_view(
		const glm::mat4& view,
		const glm::mat4& projection,
		Extent2D extent,
		LayerMask layers = LAYER_ALL,
		const char* name = "view") noexcept
	{
		const glm::mat4 view_projection = projection * view;

		return {
			.view			 = view,
			.projection		 = projection,
			.view_projection = view_projection,
			.frustum		 = frustum_from(view_projection),
			.position		 = view_position(view),
			.extent			 = extent,
			.layers			 = layers,
			.name			 = name,
		};
	}

	/**
	 * Reverse Z projection with an infinite far plane. Depth clears to zero,
	 * GreaterEqual compares, and precision concentrates where scenes need it;
	 * the far plane dissapears entirely instead of being a large magic number.
	 */
	[[nodiscard]] inline glm::mat4 perspective_reverse_z(f32 fov_y, f32 aspect, f32 near) noexcept
	{
		const f32 f = 1.0f / std::tan(fov_y * 0.5f);

		glm::mat4 m(0.0f);
		m[0][0] = f / aspect;
		m[1][1] = f;
		m[2][3] = -1.0f;
		m[3][2] = near;
		return m;
	}

	/// Reverse Z orthographic projection: depth one at the near plane, zero at
	/// the far plane, matching the perspective convention so one pipeline state
	/// serves both.
	[[nodiscard]] inline glm::mat4 ortho_reverse_z(f32 half_width, f32 half_height, f32 near, f32 far) noexcept
	{
		glm::mat4 m(1.0f);
		m[0][0] = 1.0f / half_width;
		m[1][1] = 1.0f / half_height;
		m[2][2] = 1.0f / (far - near);
		m[3][2] = far / (far - near);
		return m;
	}

	struct SnappedOrthoView
	{
		View view = {};

		/// The sub-texel camera remainder, in texels. Feed it to the upscale
		/// feature so panning stays smooth while geometry stays pixel locked.
		glm::vec2 subtexel_offset = {};
	};

	/**
	 * An orthographic view whose origin snaps to the texel grid. Whole texels
	 * move the view; the remainder returns as a UV offset for the upscale
	 * pass. The half height is world units on screen; the texel size follows
	 * from it and the internal resolution.
	 */
	[[nodiscard]] inline SnappedOrthoView make_snapped_ortho_view(
		glm::vec3 position,
		glm::vec3 target,
		glm::vec3 up,
		f32 half_height,
		Extent2D internal_extent,
		f32 near,
		f32 far,
		LayerMask layers = LAYER_ALL,
		const char* name = "main") noexcept
	{
		const f32 aspect	 = static_cast<f32>(internal_extent.width) / static_cast<f32>(internal_extent.height);
		const f32 half_width = half_height * aspect;
		const f32 texel		 = (2.0f * half_height) / static_cast<f32>(internal_extent.height);

		glm::mat4 view = glm::lookAt(position, target, up);

		// Snap the view space translation so world geometry lands on texel
		// centers regardless of camera position.
		const glm::vec2 translation{view[3].x, view[3].y};
		const glm::vec2 snapped = glm::floor(translation / texel + 0.5f) * texel;

		view[3].x = snapped.x;
		view[3].y = snapped.y;

		return {
			.view = make_view(view, ortho_reverse_z(half_width, half_height, near, far), internal_extent, layers, name),
			.subtexel_offset = (translation - snapped) / texel,
		};
	}

}
