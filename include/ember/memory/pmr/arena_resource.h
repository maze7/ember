#pragma once

#include <ember/memory/memory.h>
#include <ember/core/bits.h>
#include <ember/sync/thread.h>
#include <ember/memory/common.h>

#include <cstddef>
#include <memory_resource>

namespace ember
{
	// Linear allocator over a large virtual-memory reservation, committed in steps as it
	// fills. Memory is reclaimed wholesale via reset()/ArenaScope, never individually —
	// do_deallocate is a no-op. PMR containers pointed at an arena allocate in O(1) and
	// free for free, at the cost of memory staying live until the next reset.
	//
	// Frame-arena allocations are valid until the next reset; never cache them across
	// frames. The arena is single-threaded and owner-asserted after init(). If cross-frame
	// scratch survives one frame in practice, use a double-buffered pair of arenas instead.
	//
	// Arena allocations are not tracked; per-frame churn would spam the tracker. Peak and
	// committed sizes appear in tracker::report().
	class ArenaResource final : public std::pmr::memory_resource
	{
	public:
		struct Marker
		{
			size_t offset = 0;
		};

		constexpr ArenaResource() noexcept = default;
		~ArenaResource() noexcept override;

		ArenaResource(const ArenaResource&)			   = delete;
		ArenaResource& operator=(const ArenaResource&) = delete;

		[[nodiscard]] bool init(size_t reserve_size, size_t commit_step,
								MemoryTag tag = MemoryTag::Engine) noexcept;
		void shutdown() noexcept;

		// Bump-allocation fast path: public and non-virtual so hot loops holding the
		// concrete type always inline it (PMR containers reach the same code through
		// do_allocate). Zero-size allocations consume one byte so returned pointers are
		// valid and unique until reset.
		[[nodiscard]] void* allocate_fast(size_t size, size_t alignment = DEFAULT_ALIGNMENT) noexcept
		{
			EMBER_ASSERT(current_thread_id() == m_owner_thread);
			EMBER_ASSERT(is_power_of_two(alignment));

			const size_t allocation_size = size == 0 ? 1 : size;
			const size_t aligned		 = align_up(m_offset, alignment);
			size_t next					 = 0;
			if (!checked_add(aligned, allocation_size, next)) [[unlikely]]
				out_of_memory(size, alignment, m_tag);

			if (next > m_committed) [[unlikely]]
				if (!grow(next))
					out_of_memory(size, alignment, m_tag);

			m_offset = next;
			m_peak	 = m_offset > m_peak ? m_offset : m_peak;
			return m_base + aligned;
		}

		[[nodiscard]] Marker mark() const noexcept
		{
			return {m_offset};
		}

		void reset() noexcept
		{
			reset(Marker{});
		}

		void reset(Marker marker) noexcept;

		// True if ptr lies inside this arena's reservation. Loose on purpose (checks the
		// reserve, not the live range) so it stays valid across resets; used to catch
		// blocks from another resource being freed through this one.
		[[nodiscard]] bool owns(const void* ptr) const noexcept
		{
			const u8* p = static_cast<const u8*>(ptr);
			return p >= m_base && p < m_base + m_reserved;
		}

		[[nodiscard]] size_t used() const noexcept { return m_offset; }
		[[nodiscard]] size_t committed() const noexcept { return m_committed; }
		[[nodiscard]] size_t peak() const noexcept { return m_peak; }
		[[nodiscard]] size_t reserved() const noexcept { return m_reserved; }

	private:
		void* do_allocate(size_t bytes, size_t alignment) noexcept override
		{
			return allocate_fast(bytes, alignment);
		}

		void do_deallocate(void* ptr, size_t /*bytes*/, size_t /*alignment*/) noexcept override
		{
			EMBER_ASSERT(ptr == nullptr || owns(ptr));
			(void)ptr; // Reclaimed wholesale by reset().
		}

		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
		{
			return this == &other; // Each arena is its own memory space.
		}

		[[nodiscard]] bool grow(size_t required) noexcept;

		u8*		  m_base		 = nullptr;
		size_t	  m_reserved	 = 0;
		size_t	  m_committed	 = 0;
		size_t	  m_offset		 = 0;
		size_t	  m_peak		 = 0;
		size_t	  m_commit_step	 = 0;
		u32		  m_owner_thread = 0;
		MemoryTag m_tag			 = MemoryTag::Unknown;
	};

	class ArenaScope
	{
	public:
		explicit ArenaScope(ArenaResource& arena) noexcept : m_arena(arena), m_marker(arena.mark()) {}
		~ArenaScope() noexcept
		{
			m_arena.reset(m_marker);
		}

		ArenaScope(const ArenaScope&)			 = delete;
		ArenaScope& operator=(const ArenaScope&) = delete;

	private:
		ArenaResource&		  m_arena;
		ArenaResource::Marker m_marker;
	};
}
