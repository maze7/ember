#pragma once

#include <ember/core/common.h>

#include <glm/vec2.hpp>

/**
 * The shapes things collide with: boxes and circles, measured in world units from an entity's
 * position, +x east and +y south, as the art is. Plain values a prefab can hold. A box is drawn
 * facing east and can be turned to face any way, by a direction and never an angle, so no machine's
 * sine differs from another's.
 */
namespace ember::physics
{
	/** An upright rectangle: what the broad phase sorts, and what a tile is. */
	struct Aabb
	{
		glm::vec2 min = {};
		glm::vec2 max = {};
	};

	/** Whether two rectangles share area. Touching edges do not. */
	[[nodiscard]] constexpr bool overlaps(const Aabb& a, const Aabb& b) noexcept
	{
		return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y;
	}

	enum class ShapeKind : u8
	{
		Box,
		Circle,
		Count
	};

	struct Shape
	{
		ShapeKind kind	 = ShapeKind::Box;
		glm::vec2 center = {};			 // from the entity's position
		glm::vec2 half	 = {};			 // a box's half size; a circle's radius in x
		glm::vec2 axis	 = {1.0f, 0.0f}; // the way a box's own +x points, at unit length: east is upright

		/** The same shape about a position: an entity's shape in the world. */
		[[nodiscard]] constexpr Shape at(glm::vec2 position) const noexcept
		{
			Shape placed  = *this;
			placed.center = {center.x + position.x, center.y + position.y};
			return placed;
		}

		/**
		 * The same shape turned from facing east to facing `direction`, a unit vector, about the entity:
		 * a sword's reach, authored once pointing east, turned to the aim.
		 */
		[[nodiscard]] constexpr Shape turned(glm::vec2 direction) const noexcept
		{
			Shape result  = *this;
			result.center = {center.x * direction.x - center.y * direction.y,
							 center.x * direction.y + center.y * direction.x};
			result.axis	  = {axis.x * direction.x - axis.y * direction.y, axis.x * direction.y + axis.y * direction.x};
			return result;
		}

		[[nodiscard]] constexpr bool upright() const noexcept { return axis.x == 1.0f && axis.y == 0.0f; }
	};

	/** A box of a size, its middle at `center`. */
	[[nodiscard]] constexpr Shape box(glm::vec2 size, glm::vec2 center = {}) noexcept
	{
		return {.kind = ShapeKind::Box, .center = center, .half = {size.x * 0.5f, size.y * 0.5f}, .axis = {1.0f, 0.0f}};
	}

	[[nodiscard]] constexpr Shape circle(f32 radius, glm::vec2 center = {}) noexcept
	{
		return {.kind = ShapeKind::Circle, .center = center, .half = {radius, radius}, .axis = {1.0f, 0.0f}};
	}

	/** The upright rectangle around a shape. */
	[[nodiscard]] constexpr Aabb bounds(const Shape& shape) noexcept
	{
		glm::vec2 reach = shape.half;
		if (shape.kind == ShapeKind::Box && !shape.upright())
		{
			const glm::vec2 a{shape.axis.x < 0.0f ? -shape.axis.x : shape.axis.x,
							  shape.axis.y < 0.0f ? -shape.axis.y : shape.axis.y};
			reach = {a.x * shape.half.x + a.y * shape.half.y, a.y * shape.half.x + a.x * shape.half.y};
		}
		return {{shape.center.x - reach.x, shape.center.y - reach.y}, {shape.center.x + reach.x, shape.center.y + reach.y}};
	}

	/** Whether two shapes, both placed in the world, share area. Touching edges do not. */
	[[nodiscard]] bool overlaps(const Shape& a, const Shape& b) noexcept;

	/** Whether a placed shape holds a point. */
	[[nodiscard]] bool contains(const Shape& shape, glm::vec2 point) noexcept;
}
