#include <ember/physics/shape.h>

#include <algorithm>
#include <cmath>

namespace ember::physics
{
	namespace
	{
		[[nodiscard]] f32 dot(glm::vec2 a, glm::vec2 b) noexcept { return a.x * b.x + a.y * b.y; }

		/** A box's own +y, a quarter turn on from its +x. */
		[[nodiscard]] glm::vec2 side(glm::vec2 axis) noexcept { return {-axis.y, axis.x}; }

		/** A point in a box's own frame: along its axis, and across it. */
		[[nodiscard]] glm::vec2 local(const Shape& box, glm::vec2 point) noexcept
		{
			const glm::vec2 from = point - box.center;
			return {dot(from, box.axis), dot(from, side(box.axis))};
		}

		[[nodiscard]] bool circle_box(const Shape& circle, const Shape& box) noexcept
		{
			// The box's point nearest the circle's middle, in the box's frame, is a clamp away.
			const glm::vec2 middle	= local(box, circle.center);
			const glm::vec2 nearest = glm::vec2(std::clamp(middle.x, -box.half.x, box.half.x),
												std::clamp(middle.y, -box.half.y, box.half.y));
			const glm::vec2 apart	= middle - nearest;
			return dot(apart, apart) < circle.half.x * circle.half.x;
		}

		/** How far a box reaches either side of its middle along a unit line. */
		[[nodiscard]] f32 reach(const Shape& box, glm::vec2 line) noexcept
		{
			return box.half.x * std::abs(dot(box.axis, line)) + box.half.y * std::abs(dot(side(box.axis), line));
		}

		[[nodiscard]] bool box_box(const Shape& a, const Shape& b) noexcept
		{
			const glm::vec2 apart = b.center - a.center;
			if (a.upright() && b.upright())
				return std::abs(apart.x) < a.half.x + b.half.x && std::abs(apart.y) < a.half.y + b.half.y;

			// Two boxes are apart when a line along a side of either has a gap between them.
			for (const glm::vec2 line : {a.axis, side(a.axis), b.axis, side(b.axis)})
				if (std::abs(dot(apart, line)) >= reach(a, line) + reach(b, line))
					return false;
			return true;
		}
	}

	bool overlaps(const Shape& a, const Shape& b) noexcept
	{
		if (a.kind == ShapeKind::Circle && b.kind == ShapeKind::Circle)
		{
			const glm::vec2 apart = b.center - a.center;
			const f32 reach		  = a.half.x + b.half.x;
			return dot(apart, apart) < reach * reach;
		}

		if (a.kind == ShapeKind::Circle)
			return circle_box(a, b);
		if (b.kind == ShapeKind::Circle)
			return circle_box(b, a);
		return box_box(a, b);
	}

	bool contains(const Shape& shape, glm::vec2 point) noexcept
	{
		if (shape.kind == ShapeKind::Circle)
		{
			const glm::vec2 from = point - shape.center;
			return dot(from, from) < shape.half.x * shape.half.x;
		}

		const glm::vec2 at = local(shape, point);
		return std::abs(at.x) < shape.half.x && std::abs(at.y) < shape.half.y;
	}
}
