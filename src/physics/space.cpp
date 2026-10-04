#include <ember/physics/space.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace ember::physics
{
	namespace
	{
		/**
		 * Slack either side of an edge. A box that rests against a wall is placed there by arithmetic that
		 * can leave it a rounding error inside, and a position that crossed the wire may be rounded
		 * again: within the skin, touching is still touching. A sixty-fourth of a texel, which nothing shows.
		 */
		constexpr f32 SKIN = 1.0f / 64.0f;

		/** A proxy over more cells than this a side is kept apart from the grid, and asked of every query. */
		constexpr i32 WIDE_CELLS = 8;

		[[nodiscard]] i32 floor_to_int(f32 value) noexcept { return static_cast<i32>(std::floor(value)); }
		[[nodiscard]] i32 ceil_to_int(f32 value) noexcept { return static_cast<i32>(std::ceil(value)); }

		[[nodiscard]] constexpr u32 cell_key(i32 x, i32 y) noexcept
		{
			return (static_cast<u32>(x) << 16) | (static_cast<u32>(y) & 0xffffu);
		}

		[[nodiscard]] constexpr u32 bucket_of(u32 key) noexcept
		{
			key *= 0x9E3779B1u;
			return key ^ (key >> 15);
		}

		/** Whether two rectangles touch or share area: what the grid hands on, for the caller to judge. */
		[[nodiscard]] constexpr bool meets(const Aabb& a, const Aabb& b) noexcept
		{
			return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y;
		}

		void add_found(Found& found, const Proxy& proxy) noexcept
		{
			if (found.count == Found::CAPACITY)
			{
				found.more = true;
				return;
			}
			found.items[found.count++] = {.entity = proxy.entity, .layers = proxy.layers};
		}
	}

	Space::Space(const SpaceDef& def) noexcept
		: m_def(def), m_inverse_tile(1.0f / def.tile_size), m_inverse_cell(1.0f / def.cell_size),
		  m_page_shift(static_cast<u32>(std::countr_zero(def.page_size)))
	{
		EMBER_ASSERT(def.tile_size > 0.0f && def.cell_size > 0.0f);
		EMBER_ASSERT(std::has_single_bit(def.page_size) && def.page_size <= 256 && "a page is a power of two tiles a side");
	}

	u32 Space::page_at(glm::ivec2 page) noexcept
	{
		const auto [found, made] = m_pages.try_emplace(page_key(page), 0u);
		if (!made)
			return found->second;

		// The place of a page let go, or a new one at the end.
		const u32 area = m_def.page_size * m_def.page_size;
		if (!m_free_pages.empty())
		{
			found->second = m_free_pages.back();
			m_free_pages.pop_back();
			std::fill_n(m_tiles.begin() + found->second, area, 0u);
		}
		else
		{
			found->second = static_cast<u32>(m_tiles.size());
			m_tiles.resize(m_tiles.size() + area, 0u);
		}
		return found->second;
	}

	void Space::load_page(glm::ivec2 page, Span<const Layers> tiles) noexcept
	{
		EMBER_ASSERT(tiles.size() == static_cast<size_t>(m_def.page_size) * m_def.page_size && "a page's every tile");

		const u32 at = page_at(page);
		for (size_t i = 0; i < tiles.size(); ++i)
			m_tiles[at + i] = tiles[i].bits;
	}

	void Space::unload_page(glm::ivec2 page) noexcept
	{
		const auto found = m_pages.find(page_key(page));
		if (found == m_pages.end())
			return;

		m_free_pages.push_back(found->second);
		m_pages.erase(found);
	}

	void Space::set_tile(glm::ivec2 tile, Layers layers) noexcept
	{
		const i32 mask = static_cast<i32>(m_def.page_size) - 1;
		const u32 at   = page_at({tile.x >> m_page_shift, tile.y >> m_page_shift});
		m_tiles[at + static_cast<u32>(((tile.y & mask) << m_page_shift) + (tile.x & mask))] = layers.bits;
	}

	void Space::clear_tiles() noexcept
	{
		m_pages.clear();
		m_tiles.clear();
		m_free_pages.clear();
	}

	Layers Space::ground(glm::vec2 point) const noexcept
	{
		return tile({floor_to_int(point.x * m_inverse_tile), floor_to_int(point.y * m_inverse_tile)});
	}

	void Space::Index::clear() noexcept
	{
		proxies.clear();
		bounds.clear();
		layers = 0;
	}

	void Space::Index::add(const Proxy& proxy) noexcept
	{
		proxies.push_back(proxy);
		bounds.push_back(physics::bounds(proxy.shape));
		layers |= proxy.layers.bits;
	}

	void Space::Index::build(f32 inverse_cell) noexcept
	{
		const u32 count = static_cast<u32>(proxies.size());

		u32 buckets = 64;
		while (buckets < count * 2)
			buckets <<= 1;
		mask = buckets - 1;

		starts.assign(static_cast<size_t>(buckets) + 1, 0);
		wide.clear();

		struct Cells
		{
			i32 x0, y0, x1, y1;
		};
		const auto cells_of = [&](const Aabb& box) -> Cells
		{
			return {floor_to_int(box.min.x * inverse_cell), floor_to_int(box.min.y * inverse_cell),
					floor_to_int(box.max.x * inverse_cell), floor_to_int(box.max.y * inverse_cell)};
		};
		const auto is_wide = [](const Cells& cells)
		{ return cells.x1 - cells.x0 >= WIDE_CELLS || cells.y1 - cells.y0 >= WIDE_CELLS; };

		// A counting sort by bucket: how many each holds, where each ends, then every entry into its place.
		u32 total = 0;
		for (u32 i = 0; i < count; ++i)
		{
			const Cells cells = cells_of(bounds[i]);
			if (is_wide(cells))
			{
				wide.push_back(i);
				continue;
			}

			for (i32 y = cells.y0; y <= cells.y1; ++y)
				for (i32 x = cells.x0; x <= cells.x1; ++x)
				{
					++starts[bucket_of(cell_key(x, y)) & mask];
					++total;
				}
		}

		u32 end = 0;
		for (u32 bucket = 0; bucket < buckets; ++bucket)
		{
			end			  += starts[bucket];
			starts[bucket] = end;
		}
		starts[buckets] = total;

		// From the last proxy back, each entry just before the one placed after it: buckets keep proxy order.
		entries.resize(total);
		for (u32 i = count; i-- > 0;)
		{
			const Cells cells = cells_of(bounds[i]);
			if (is_wide(cells))
				continue;

			for (i32 y = cells.y1; y >= cells.y0; --y)
				for (i32 x = cells.x1; x >= cells.x0; --x)
				{
					const u32 key								   = cell_key(x, y);
					entries[--starts[bucket_of(key) & mask]] = {.cell = key, .proxy = i};
				}
		}
	}

	template <class F> void Space::each(const Index& index, const Aabb& area, u32 layers, F&& fn) const noexcept
	{
		// Most questions are about layers nothing here is on: a mover that only walls stop asks no collider.
		if ((index.layers & layers) == 0)
			return;

		const i32 x0 = floor_to_int(area.min.x * m_inverse_cell), y0 = floor_to_int(area.min.y * m_inverse_cell);
		const i32 x1 = floor_to_int(area.max.x * m_inverse_cell), y1 = floor_to_int(area.max.y * m_inverse_cell);

		for (i32 y = y0; y <= y1; ++y)
		{
			for (i32 x = x0; x <= x1; ++x)
			{
				const u32 key	 = cell_key(x, y);
				const u32 bucket = bucket_of(key) & index.mask;

				for (u32 at = index.starts[bucket]; at < index.starts[bucket + 1]; ++at)
				{
					const Entry& entry = index.entries[at];
					if (entry.cell != key || (index.proxies[entry.proxy].layers.bits & layers) == 0)
						continue;

					const Aabb& box = index.bounds[entry.proxy];
					if (!meets(box, area))
						continue;

					// A proxy in several of these cells counts in one: the cell that holds the corner the two share.
					if (floor_to_int(std::max(box.min.x, area.min.x) * m_inverse_cell) != x ||
						floor_to_int(std::max(box.min.y, area.min.y) * m_inverse_cell) != y)
						continue;

					fn(entry.proxy);
				}
			}
		}

		for (const u32 proxy : index.wide)
			if ((index.proxies[proxy].layers.bits & layers) != 0 && meets(index.bounds[proxy], area))
				fn(proxy);
	}

	void Space::clear() noexcept
	{
		m_colliders.clear();
		m_hurtboxes.clear();
		m_hitboxes.clear();
	}

	void Space::add(ecs::Entity entity, glm::vec2 position, const Collider& collider) noexcept
	{
		m_colliders.add({.entity = entity, .layers = collider.layer, .shape = collider.shape.at(position)});
	}

	void Space::add(ecs::Entity entity, glm::vec2 position, const Hurtbox& hurtbox) noexcept
	{
		m_hurtboxes.add({.entity = entity, .layers = hurtbox.layer, .shape = hurtbox.shape.at(position)});
	}

	void Space::add(ecs::Entity entity, glm::vec2 position, const Hitbox& hitbox) noexcept
	{
		if (!hitbox.hits.none())
			m_hitboxes.push_back({.entity = entity, .layers = hitbox.hits, .shape = hitbox.shape.at(position)});
	}

	void Space::build() noexcept
	{
		m_colliders.build(m_inverse_cell);
		m_hurtboxes.build(m_inverse_cell);
	}

	f32 Space::slide(const Aabb& box, u32 axis, f32 delta, Layers by, ecs::Entity self, Moved& moved,
					 bool& stopped) const noexcept
	{
		const u32 across  = 1 - axis;
		const bool ahead  = delta > 0.0f;
		const f32 lead	  = ahead ? box.max[axis] : box.min[axis];
		const f32 target  = lead + delta;
		const f32 size	  = m_def.tile_size;
		f32 limit		  = target;
		Layers what;
		ecs::Entity who = ecs::NO_ENTITY;

		// The ground: the tiles the box is abreast of, column by column from its leading edge. A column the
		// edge is already inside does not count, so a box that starts in a wall walks out of it.
		const i32 low  = floor_to_int((box.min[across] + SKIN) * m_inverse_tile);
		const i32 high = floor_to_int((box.max[across] - SKIN) * m_inverse_tile);

		const auto solid = [&](i32 column) -> bool
		{
			for (i32 row = low; row <= high; ++row)
			{
				const Layers layers = tile(axis == 0 ? glm::ivec2(column, row) : glm::ivec2(row, column));
				if (layers.any(by))
				{
					what = layers;
					return true;
				}
			}
			return false;
		};

		if (ahead)
		{
			const i32 last = floor_to_int(target * m_inverse_tile);
			for (i32 column = ceil_to_int((lead - SKIN) * m_inverse_tile); column <= last; ++column)
			{
				if (solid(column))
				{
					limit = std::min(limit, static_cast<f32>(column) * size);
					break;
				}
			}
		}
		else
		{
			const i32 last = floor_to_int(target * m_inverse_tile);
			for (i32 column = floor_to_int((lead + SKIN) * m_inverse_tile) - 1; column >= last; --column)
			{
				if (solid(column))
				{
					limit = std::max(limit, static_cast<f32>(column + 1) * size);
					break;
				}
			}
		}

		// Other colliders, as the upright boxes around them: the nearest one abreast of the box and ahead of its edge.
		Aabb area		  = box;
		area.min[across] += SKIN;
		area.max[across] -= SKIN;
		area.min[axis]	  = ahead ? lead - SKIN : limit;
		area.max[axis]	  = ahead ? limit : lead + SKIN;

		each(m_colliders, area, by.bits, [&](u32 index)
			 {
				 const Proxy& proxy = m_colliders.proxies[index];
				 const Aabb& other	= m_colliders.bounds[index];
				 if (proxy.entity == self || other.max[across] <= area.min[across] || other.min[across] >= area.max[across])
					 return;

				 if (ahead && other.min[axis] >= lead - SKIN && other.min[axis] < limit)
				 {
					 limit = other.min[axis];
					 who   = proxy.entity;
					 what  = proxy.layers;
				 }
				 else if (!ahead && other.max[axis] <= lead + SKIN && other.max[axis] > limit)
				 {
					 limit = other.max[axis];
					 who   = proxy.entity;
					 what  = proxy.layers;
				 }
			 });

		stopped = limit != target;
		if (stopped)
		{
			moved.normal[static_cast<glm::length_t>(axis)] = ahead ? -1.0f : 1.0f;
			moved.layers.bits							  |= what.bits;
			if (who != ecs::NO_ENTITY)
				moved.entity = who;
		}
		return limit;
	}

	Moved Space::move(ecs::Entity self, const Collider& collider, glm::vec2 position, glm::vec2 delta) const noexcept
	{
		const Shape& shape = collider.shape;
		EMBER_ASSERT(shape.kind == ShapeKind::Box && shape.upright() && "what walls stop is an upright box");

		Moved moved;
		moved.position = position;
		if (collider.blocked_by.none())
		{
			moved.position = position + delta;
			return moved;
		}

		// East and west first, then north and south from where that got to: each exact, and a wall's seams
		// are never met, because the tiles ahead on one axis are only those the box is abreast of.
		for (glm::length_t axis = 0; axis < 2; ++axis)
		{
			if (delta[axis] == 0.0f)
				continue;

			bool stopped   = false;
			const f32 edge = slide(bounds(shape.at(moved.position)), static_cast<u32>(axis), delta[axis],
								   collider.blocked_by, self, moved, stopped);

			// Stopped, the position that puts its leading edge on what stopped it. Free, the move as asked,
			// to the bit.
			if (stopped)
				moved.position[axis] = edge - shape.center[axis] - (delta[axis] > 0.0f ? shape.half[axis] : -shape.half[axis]);
			else
				moved.position[axis] += delta[axis];
		}

		return moved;
	}

	bool Space::blocked(const Shape& shape, Layers by, ecs::Entity ignore) const noexcept
	{
		const Aabb area = bounds(shape);

		const i32 x0 = floor_to_int((area.min.x + SKIN) * m_inverse_tile), y0 = floor_to_int((area.min.y + SKIN) * m_inverse_tile);
		const i32 x1 = floor_to_int((area.max.x - SKIN) * m_inverse_tile), y1 = floor_to_int((area.max.y - SKIN) * m_inverse_tile);
		for (i32 y = y0; y <= y1; ++y)
		{
			for (i32 x = x0; x <= x1; ++x)
			{
				if (!tile({x, y}).any(by))
					continue;

				const glm::vec2 corner{static_cast<f32>(x) * m_def.tile_size, static_cast<f32>(y) * m_def.tile_size};
				if (overlaps(shape, box(glm::vec2(m_def.tile_size), corner + glm::vec2(m_def.tile_size * 0.5f))))
					return true;
			}
		}

		bool found = false;
		each(m_colliders, area, by.bits, [&](u32 index)
			 {
				 const Proxy& proxy = m_colliders.proxies[index];
				 found = found || (proxy.entity != ignore && overlaps(shape, proxy.shape));
			 });
		return found;
	}

	Found Space::touching(const Index& index, const Shape& shape, Layers layers) const noexcept
	{
		Found found;
		each(index, bounds(shape), layers.bits, [&](u32 at)
			 {
				 if (overlaps(shape, index.proxies[at].shape))
					 add_found(found, index.proxies[at]);
			 });

		// In entity order, whatever order the grid gave them in.
		std::sort(found.items, found.items + found.count,
				  [](const Touch& a, const Touch& b) { return entt::to_integral(a.entity) < entt::to_integral(b.entity); });
		return found;
	}

	void Space::hurtboxes(const Shape& shape, Layers layers, Vector<Touch>& out) const noexcept
	{
		const size_t first = out.size();
		each(m_hurtboxes, bounds(shape), layers.bits, [&](u32 at)
			 {
				 const Proxy& proxy = m_hurtboxes.proxies[at];
				 if (overlaps(shape, proxy.shape))
					 out.push_back({.entity = proxy.entity, .layers = proxy.layers});
			 });

		std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
				  [](const Touch& a, const Touch& b) { return entt::to_integral(a.entity) < entt::to_integral(b.entity); });
	}

	Found Space::colliders(const Shape& shape, Layers layers) const noexcept { return touching(m_colliders, shape, layers); }
	Found Space::hurtboxes(const Shape& shape, Layers layers) const noexcept { return touching(m_hurtboxes, shape, layers); }

	std::optional<RayHit> Space::raycast(glm::vec2 from, glm::vec2 to, Layers layers, ecs::Entity ignore) const noexcept
	{
		const glm::vec2 line = to - from;
		const f32 length	 = std::sqrt(line.x * line.x + line.y * line.y);
		f32 nearest			 = std::numeric_limits<f32>::max(); // along the line, 0 at `from` and 1 at `to`
		RayHit hit;

		// The ground, tile by tile along the line (Amanatides and Woo).
		{
			const f32 size = m_def.tile_size;
			glm::ivec2 at{floor_to_int(from.x * m_inverse_tile), floor_to_int(from.y * m_inverse_tile)};
			const glm::ivec2 step{line.x > 0.0f ? 1 : -1, line.y > 0.0f ? 1 : -1};

			const f32 never = std::numeric_limits<f32>::max();
			glm::vec2 next{line.x != 0.0f ? (static_cast<f32>(at.x + (step.x > 0 ? 1 : 0)) * size - from.x) / line.x : never,
						   line.y != 0.0f ? (static_cast<f32>(at.y + (step.y > 0 ? 1 : 0)) * size - from.y) / line.y : never};
			const glm::vec2 stride{line.x != 0.0f ? size / std::abs(line.x) : never,
								   line.y != 0.0f ? size / std::abs(line.y) : never};

			f32 along = 0.0f;
			glm::vec2 normal{};
			while (along <= 1.0f)
			{
				if (const Layers ground = tile(at); ground.any(layers))
				{
					nearest = along;
					hit		= {.distance = along * length, .point = from + line * along, .normal = normal, .entity = ecs::NO_ENTITY, .layers = ground};
					break;
				}

				if (next.x < next.y)
				{
					along	= next.x;
					next.x += stride.x;
					at.x   += step.x;
					normal	= {static_cast<f32>(-step.x), 0.0f};
				}
				else
				{
					along	= next.y;
					next.y += stride.y;
					at.y   += step.y;
					normal	= {0.0f, static_cast<f32>(-step.y)};
				}
			}
		}

		// Colliders, as the upright boxes around them: where the line enters each.
		const Aabb area{{std::min(from.x, to.x), std::min(from.y, to.y)}, {std::max(from.x, to.x), std::max(from.y, to.y)}};
		each(m_colliders, area, layers.bits, [&](u32 index)
			 {
				 const Proxy& proxy = m_colliders.proxies[index];
				 if (proxy.entity == ignore)
					 return;

				 const Aabb& other = m_colliders.bounds[index];
				 f32 enter = 0.0f, leave = 1.0f;
				 glm::vec2 normal{};
				 for (glm::length_t axis = 0; axis < 2; ++axis)
				 {
					 if (line[axis] == 0.0f)
					 {
						 if (from[axis] <= other.min[axis] || from[axis] >= other.max[axis])
							 return;
						 continue;
					 }

					 f32 near = (other.min[axis] - from[axis]) / line[axis];
					 f32 far  = (other.max[axis] - from[axis]) / line[axis];
					 if (near > far)
						 std::swap(near, far);
					 if (near > enter)
					 {
						 enter		  = near;
						 normal		  = {};
						 normal[axis] = line[axis] > 0.0f ? -1.0f : 1.0f;
					 }
					 leave = std::min(leave, far);
				 }

				 if (enter <= leave && enter < nearest)
				 {
					 nearest = enter;
					 hit	 = {.distance = enter * length, .point = from + line * enter, .normal = normal, .entity = proxy.entity, .layers = proxy.layers};
				 }
			 });

		if (nearest > 1.0f)
			return std::nullopt;
		return hit;
	}
}
