#pragma once

#include <ember/containers/span.h>
#include <ember/ecs/components.h>
#include <ember/memory/memory.h>
#include <ember/physics/components.h>

#include <glm/ext/vector_int2.hpp>

#include <optional>

namespace ember::physics
{
	struct SpaceDef
	{
		f32 tile_size = 16.0f; // world units a tile
		f32 cell_size = 64.0f; // the broad phase's grid: a few times the size of what it sorts
		u32 page_size = 16;	   // tiles a side of a page of ground, a power of two: the game's chunk
		Layers outside = {};   // what the ground is where no page is loaded: nothing, or a wall about the world
	};

	/** Where a move ended, and what stopped it. */
	struct Moved
	{
		glm::vec2 position = {};
		glm::vec2 normal   = {};			 // away from what stopped it, on each axis that was stopped: zero when free
		ecs::Entity entity = ecs::NO_ENTITY; // the collider that stopped it; none for tiles, or nothing
		Layers layers = {};					 // what stopped it

		[[nodiscard]] bool blocked() const noexcept { return normal.x != 0.0f || normal.y != 0.0f; }
	};

	/** What a ray met first. */
	struct RayHit
	{
		f32 distance	   = 0.0f;
		glm::vec2 point	   = {};
		glm::vec2 normal   = {};
		ecs::Entity entity = ecs::NO_ENTITY; // none for a tile
		Layers layers = {};
	};

	/** One thing a query found. */
	struct Touch
	{
		ecs::Entity entity = ecs::NO_ENTITY;
		Layers layers = {};
	};

	/** What a query found, in entity order: up to CAPACITY, and whether there were more. */
	struct Found
	{
		static constexpr u32 CAPACITY = 32;

		Touch items[CAPACITY];
		u32 count = 0;
		bool more = false;

		[[nodiscard]] const Touch* begin() const noexcept { return items; }
		[[nodiscard]] const Touch* end() const noexcept { return items + count; }
		[[nodiscard]] bool empty() const noexcept { return count == 0; }
	};

	/** A collider, hurtbox or hitbox as the space holds it: whose it is, and its shape in the world. */
	struct Proxy
	{
		ecs::Entity entity = ecs::NO_ENTITY;
		Layers layers = {};
		Shape shape;
	};

	/**
	 * Everything in a world that collides, sorted so a question costs what is near it: the ground's
	 * tiles, and every entity's collider, hurtbox and hitbox where it stood when the tick's sync ran.
	 * A world resource. Systems take it const to move things and ask questions, which any number may
	 * do at once; only the sync writes it.
	 *
	 * Nothing here remembers a tick: a move and every query depend on the space and their arguments
	 * alone, so a client replaying its prediction gets what it got the first time.
	 */
	class Space final
	{
	public:
		explicit Space(const SpaceDef& def = {}) noexcept;

		Space(const Space&)			   = delete;
		Space& operator=(const Space&) = delete;

		[[nodiscard]] const SpaceDef& def() const noexcept { return m_def; }

		// The ground, a page of page_size tiles a side at a time: the game loads a page as its world streams
		// in about where things are, and lets it go as the world streams out. Where no page is loaded the
		// ground is SpaceDef::outside, however far the world goes.

		/** A page's tiles, row by row, page_size squared of them: in place of any it had. */
		void load_page(glm::ivec2 page, Span<const Layers> tiles) noexcept;

		/** A page goes, and the ground there is outside again. Nothing when none is loaded there. */
		void unload_page(glm::ivec2 page) noexcept;

		/** One tile, the game's edit: a page where none is loaded starts as open ground. */
		void set_tile(glm::ivec2 tile, Layers layers) noexcept;

		/** Every page goes. */
		void clear_tiles() noexcept;

		[[nodiscard]] Layers tile(glm::ivec2 tile) const noexcept
		{
			const auto page = m_pages.find(page_key({tile.x >> m_page_shift, tile.y >> m_page_shift}));
			if (page == m_pages.end())
				return m_def.outside;

			const i32 mask = static_cast<i32>(m_def.page_size) - 1;
			Layers layers;
			layers.bits = m_tiles[page->second + static_cast<u32>(((tile.y & mask) << m_page_shift) + (tile.x & mask))];
			return layers;
		}

		[[nodiscard]] u32 page_count() const noexcept { return static_cast<u32>(m_pages.size()); }

		/** The layers of the ground under a point: what a bullet asks every tick. */
		[[nodiscard]] Layers ground(glm::vec2 point) const noexcept;

		/**
		 * Moves a collider from `position` by `delta`, stopping at whatever blocks it and sliding along
		 * it. Exact however far the move: it never passes through or ends inside what blocks it, and
		 * never catches on the seam between two tiles. `self` is the mover, which does not block itself.
		 * A collider that starts inside something is not blocked by it, so nothing is ever stuck.
		 */
		[[nodiscard]] Moved move(ecs::Entity self, const Collider& collider, glm::vec2 position,
								 glm::vec2 delta) const noexcept;

		/** Whether a placed shape touches ground or a collider on these layers: is there room to stand here? */
		[[nodiscard]] bool blocked(const Shape& shape, Layers by, ecs::Entity ignore = ecs::NO_ENTITY) const noexcept;

		/** The colliders, or the hurtboxes, on these layers that a placed shape touches. */
		[[nodiscard]] Found colliders(const Shape& shape, Layers layers) const noexcept;
		[[nodiscard]] Found hurtboxes(const Shape& shape, Layers layers) const noexcept;

		/** The same, however many: added to `out` in entity order. */
		void hurtboxes(const Shape& shape, Layers layers, Vector<Touch>& out) const noexcept;

		/** The first ground or collider on these layers along a line: can this see that? */
		[[nodiscard]] std::optional<RayHit> raycast(glm::vec2 from, glm::vec2 to, Layers layers,
													ecs::Entity ignore = ecs::NO_ENTITY) const noexcept;

		// The sync, which the physics systems run once a tick: everything out, everything back in where it stands now.

		void clear() noexcept;
		void add(ecs::Entity entity, glm::vec2 position, const Collider& collider) noexcept;
		void add(ecs::Entity entity, glm::vec2 position, const Hurtbox& hurtbox) noexcept;
		void add(ecs::Entity entity, glm::vec2 position, const Hitbox& hitbox) noexcept;
		void build() noexcept;

		[[nodiscard]] Span<const Proxy> hitboxes() const noexcept { return {m_hitboxes.data(), m_hitboxes.size()}; }
		[[nodiscard]] u32 collider_count() const noexcept { return static_cast<u32>(m_colliders.proxies.size()); }
		[[nodiscard]] u32 hurtbox_count() const noexcept { return static_cast<u32>(m_hurtboxes.proxies.size()); }

		/** What the debug view draws: every proxy, and the solid tiles in a rectangle. */
		[[nodiscard]] Span<const Proxy> collider_proxies() const noexcept
		{
			return {m_colliders.proxies.data(), m_colliders.proxies.size()};
		}
		[[nodiscard]] Span<const Proxy> hurtbox_proxies() const noexcept
		{
			return {m_hurtboxes.proxies.data(), m_hurtboxes.proxies.size()};
		}

	private:
		/** One cell a proxy lies in. */
		struct Entry
		{
			u32 cell  = 0;
			u32 proxy = 0;
		};

		/**
		 * Proxies on a uniform grid, hashed, so the world has no edges: rebuilt whole each tick by a
		 * counting sort, with nothing allocated once its vectors have grown.
		 */
		struct Index
		{
			Vector<Proxy> proxies{&memory::heap(MemoryTag::Physics)};
			Vector<Aabb> bounds{&memory::heap(MemoryTag::Physics)};
			Vector<Entry> entries{&memory::heap(MemoryTag::Physics)};
			Vector<u32> starts{&memory::heap(MemoryTag::Physics)}; // by bucket: its first entry; one past the end last
			Vector<u32> wide{&memory::heap(MemoryTag::Physics)};   // proxies over too many cells to list in each
			u32 mask   = 0;										   // buckets - 1
			u32 layers = 0;										   // every layer a proxy in it is on

			void clear() noexcept;
			void add(const Proxy& proxy) noexcept;
			void build(f32 inverse_cell) noexcept;
		};

		/** Calls fn(index) for each proxy on `layers` whose bounds touch `area`, once each. */
		template <class F> void each(const Index& index, const Aabb& area, u32 layers, F&& fn) const noexcept;

		[[nodiscard]] Found touching(const Index& index, const Shape& shape, Layers layers) const noexcept;

		/** How far along one axis a box gets before what blocks it: the coordinate its leading edge reaches. */
		[[nodiscard]] f32 slide(const Aabb& box, u32 axis, f32 delta, Layers by, ecs::Entity self, Moved& moved,
								bool& stopped) const noexcept;

		[[nodiscard]] static u64 page_key(glm::ivec2 page) noexcept
		{
			return (static_cast<u64>(static_cast<u32>(page.x)) << 32) | static_cast<u32>(page.y);
		}

		/** Where a page keeps its tiles in m_tiles: the one loaded there, or a new one, open ground. */
		[[nodiscard]] u32 page_at(glm::ivec2 page) noexcept;

		SpaceDef m_def;
		f32 m_inverse_tile = 0.0f;
		f32 m_inverse_cell = 0.0f;
		u32 m_page_shift   = 0; // log2 of page_size: a tile's page is its coordinates shifted down by this

		// The ground: every page's tiles, row by row, page_size squared apiece, and where each page's begin.
		// A page let go leaves its place to the next one loaded.
		HashMap<u64, u32> m_pages{&memory::heap(MemoryTag::Physics)};
		Vector<u32> m_tiles{&memory::heap(MemoryTag::Physics)};
		Vector<u32> m_free_pages{&memory::heap(MemoryTag::Physics)};

		Index m_colliders;
		Index m_hurtboxes;
		Vector<Proxy> m_hitboxes{&memory::heap(MemoryTag::Physics)};
	};
}
