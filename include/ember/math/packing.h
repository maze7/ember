#pragma once

#include <ember/core/common.h>

#include <glm/packing.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cmath>

namespace ember
{
	/**
	 * Octahedral unit-vector packing (Cigolle et al.), two snorm16 components in a
	 * u32. Chosen for the mesh attribute stream because it is uniform over the
	 * sphere and decodes with a handful of ALU ops; the shader-side decode lives
	 * with the vertex pulling code.
	 */
	[[nodiscard]] inline u32 pack_octahedral(glm::vec3 normal) noexcept
	{
		const f32 norm = std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
		glm::vec2 projected{normal.x / norm, normal.y / norm};

		if (normal.z < 0.0f)
		{
			// Fold the lower hemisphere over the diagonals.
			const f32 x = (1.0f - std::abs(projected.y)) * (projected.x >= 0.0f ? 1.0f : -1.0f);
			const f32 y = (1.0f - std::abs(projected.x)) * (projected.y >= 0.0f ? 1.0f : -1.0f);

			projected = {x, y};
		}

		return glm::packSnorm2x16(projected);
	}

	[[nodiscard]] inline glm::vec3 unpack_octahedral(u32 packed) noexcept
	{
		const glm::vec2 projected = glm::unpackSnorm2x16(packed);

		glm::vec3 normal{projected.x, projected.y, 1.0f - std::abs(projected.x) - std::abs(projected.y)};

		if (normal.z < 0.0f)
		{
			const f32 x = (1.0f - std::abs(normal.y)) * (normal.x >= 0.0f ? 1.0f : -1.0f);
			const f32 y = (1.0f - std::abs(normal.x)) * (normal.y >= 0.0f ? 1.0f : -1.0f);

			normal.x = x;
			normal.y = y;
		}

		const f32 length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
		return normal * (1.0f / length);
	}
}
