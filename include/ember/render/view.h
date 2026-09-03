#pragma once

#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/render/common.h>

#include <glm/geometric.hpp>
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
}
