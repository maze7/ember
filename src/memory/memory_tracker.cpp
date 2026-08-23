#include <ember/core/logger.h>
#include <ember/core/common.h>
#include <ember/memory/memory.h>
#include <ember/memory/memory_tracker.h>
#include <ember/sync/spin_mutex.h>
#include <ember/memory/pmr/arena_resource.h>

#if EMBER_MEMORY_TRACKING >= 1
	#include <array>
	#include <atomic>
	#include <cstddef>
	#include <cstring>
	#include <ember/core/profile.h>
	#include <mutex>
	#include <source_location>

	#if EMBER_MEMORY_TRACKING >= 2
		#include <ankerl/unordered_dense.h>
		#include <cstdlib>
		#include <limits>
		#include <vector>
	#endif
#endif

#if EMBER_MEMORY_TRACKING >= 1
namespace ember
{
	namespace
	{
		constexpr size_t k_tag_count = static_cast<size_t>(MemoryTag::Count);

		struct alignas(EMBER_CACHE_LINE) Counters
		{
			std::atomic<u64> current_bytes = 0;
			std::atomic<u64> peak_bytes	   = 0;
			std::atomic<u64> current_count = 0;
			std::atomic<u64> total_count   = 0;
		};

		// Global (untagged) heap stats — the only counters maintained at level 1, where on_free's
		// size argument supplies the decrement. Per-tag stats need the live map's record (level 2).
		constinit Counters s_total_counters;

		// Re-entrancy guard: the tracker's own logging allocates through the tracked heap.
		thread_local bool t_in_tracker = false;

		class TrackerScope
		{
		public:
			TrackerScope() noexcept
			{
				EMBER_ASSERT(!t_in_tracker);
				t_in_tracker = true;
			}

			~TrackerScope() noexcept { t_in_tracker = false; }

			TrackerScope(const TrackerScope&)			 = delete;
			TrackerScope& operator=(const TrackerScope&) = delete;
		};

		[[nodiscard]] MemoryTag sanitize_tag(MemoryTag tag) noexcept
		{
			if (static_cast<size_t>(tag) >= k_tag_count) [[unlikely]]
				return MemoryTag::Unknown;

			return tag;
		}

		void update_peak(std::atomic<u64>& peak, u64 value) noexcept
		{
			u64 observed = peak.load(std::memory_order_relaxed);
			while (observed < value &&
				   !peak.compare_exchange_weak(observed, value, std::memory_order_relaxed, std::memory_order_relaxed))
			{
			}
		}

		// Returns the bytes held before this allocation (used for budget-crossing checks).
		u64 increment_counters(Counters& counters, u64 size) noexcept
		{
			const u64 previous_bytes = counters.current_bytes.fetch_add(size, std::memory_order_relaxed);
			(void)counters.current_count.fetch_add(1, std::memory_order_relaxed);
			(void)counters.total_count.fetch_add(1, std::memory_order_relaxed);
			update_peak(counters.peak_bytes, previous_bytes + size);
			return previous_bytes;
		}

#if EMBER_MEMORY_TRACKING >= 2
		void decrement_counters(Counters& counters, u64 size) noexcept
		{
			(void)counters.current_bytes.fetch_sub(size, std::memory_order_relaxed);
			(void)counters.current_count.fetch_sub(1, std::memory_order_relaxed);
		}
#endif

#if EMBER_MEMORY_TRACKING == 1
		void decrement_atomic_saturating(std::atomic<u64>& counter, u64 amount) noexcept
		{
			u64 observed = counter.load(std::memory_order_relaxed);
			while (observed != 0)
			{
				const u64 desired = observed > amount ? observed - amount : 0;
				if (counter.compare_exchange_weak(
						observed, desired, std::memory_order_relaxed, std::memory_order_relaxed))
					return;
			}
		}

		void decrement_counters_saturating(Counters& counters, u64 size) noexcept
		{
			// Level 1 has no membership check; guarded allocations freed outside the guard
			// must clamp the counters instead of wrapping them.
			decrement_atomic_saturating(counters.current_bytes, size);
			decrement_atomic_saturating(counters.current_count, 1);
		}
#endif

		[[nodiscard]] memory_tracker::TagStats read_counters(const Counters& counters) noexcept
		{
			return {
				.current_bytes = counters.current_bytes.load(std::memory_order_relaxed),
				.peak_bytes	   = counters.peak_bytes.load(std::memory_order_relaxed),
				.current_count = counters.current_count.load(std::memory_order_relaxed),
				.total_count   = counters.total_count.load(std::memory_order_relaxed),
			};
		}

	#if EMBER_MEMORY_TRACKING >= 2
		// The tracker's bookkeeping must not flow through the tracked heap: std::malloc
		// keeps it invisible to the stats and immune to recursion. Debug-only code, so
		// raw malloc performance is irrelevant.
		template <typename T>
		class UntrackedAllocator
		{
			static_assert(alignof(T) <= alignof(std::max_align_t));

		public:
			using value_type = T;

			constexpr UntrackedAllocator() noexcept = default;

			template <typename U>
			constexpr UntrackedAllocator(const UntrackedAllocator<U>&) noexcept
			{
			}

			[[nodiscard]] T* allocate(size_t count)
			{
				if (count > std::numeric_limits<size_t>::max() / sizeof(T)) [[unlikely]]
					out_of_memory(count, alignof(T), MemoryTag::Tools);

				void* ptr = std::malloc(count * sizeof(T));
				if (ptr == nullptr) [[unlikely]]
					out_of_memory(count * sizeof(T), alignof(T), MemoryTag::Tools);

				return static_cast<T*>(ptr);
			}

			void deallocate(T* ptr, size_t) noexcept { std::free(ptr); }
		};

		template <typename T, typename U>
		[[nodiscard]] constexpr bool operator==(const UntrackedAllocator<T>&, const UntrackedAllocator<U>&) noexcept
		{
			return true;
		}

		struct AllocationRecord
		{
			u64		  size			= 0;
			MemoryTag tag			= MemoryTag::Unknown;
			u64		  allocation_id = 0;
		};

		using AllocationMap = ankerl::unordered_dense::map<
			void*,
			AllocationRecord,
			ankerl::unordered_dense::hash<void*>,
			std::equal_to<void*>,
			UntrackedAllocator<std::pair<void*, AllocationRecord>>>;

		constinit std::atomic<bool> s_enabled			 = false;
		constinit std::atomic<u64>	s_next_allocation_id = 1;
		constinit std::atomic<u64>	s_break_allocation_id = 0;
		constinit SpinMutex	s_allocation_mutex;
		alignas(AllocationMap) std::byte s_allocation_storage[sizeof(AllocationMap)];
		AllocationMap* s_allocations = nullptr;

		constinit std::array<Counters, k_tag_count>			 s_tag_counters;
		constinit std::array<std::atomic<bool>, k_tag_count> s_budget_warned;

		[[nodiscard]] size_t tag_index(MemoryTag tag) noexcept { return static_cast<size_t>(sanitize_tag(tag)); }

		[[nodiscard]] const char* tag_name(MemoryTag tag) noexcept
		{
			const auto names = enum_names<MemoryTag>();
			return names[tag_index(tag)];
		}

		void maybe_warn_budget(MemoryTag tag, u64 previous_bytes, u64 current_bytes) noexcept
		{
			const size_t index = tag_index(tag);
			const u64 budget   = static_cast<u64>(MEMORY_TAG_BUDGETS[index]);

			if (budget == 0 || previous_bytes >= budget || current_bytes < budget)
				return;

			if (s_budget_warned[index].exchange(true, std::memory_order_relaxed))
				return;

			TrackerScope scope;
			// Memory diagnostics bypass stripped log macros: tracking-gated or fatal, and too rare to strip.
			Logger::warn(
				std::source_location::current(),
				"Memory tag {} crossed budget: current={} budget={}",
				tag_name(tag),
				current_bytes,
				budget);
		}

		void reset_counters(Counters& counters) noexcept
		{
			counters.current_bytes.store(0, std::memory_order_relaxed);
			counters.peak_bytes.store(0, std::memory_order_relaxed);
			counters.current_count.store(0, std::memory_order_relaxed);
			counters.total_count.store(0, std::memory_order_relaxed);
		}

		void reset_all_counters() noexcept
		{
			reset_counters(s_total_counters);

			for (Counters& counters : s_tag_counters)
				reset_counters(counters);

			for (std::atomic<bool>& warned : s_budget_warned)
				warned.store(false, std::memory_order_relaxed);
		}

		void erase_allocation(void* ptr, bool poison) noexcept
		{
			AllocationRecord record;

			{
				TrackerScope scope;
				std::lock_guard lock(s_allocation_mutex);

				if (s_allocations == nullptr)
					return;

				const auto it = s_allocations->find(ptr);
				if (it == s_allocations->end())
					return;

				record = it->second;
				s_allocations->erase(it);
			}

			// Once erased from the map the block is exclusively owned by the freeing thread until the
			// underlying free — poison outside the lock so a large memset never stalls other threads.
			if (poison)
				std::memset(ptr, 0xDD, static_cast<size_t>(record.size));

			EMBER_PROFILE_FREE(ptr);

			decrement_counters(s_total_counters, record.size);
			decrement_counters(s_tag_counters[tag_index(record.tag)], record.size);
		}
	#endif

		[[nodiscard]] bool tracking_enabled() noexcept
		{
	#if EMBER_MEMORY_TRACKING >= 2
			return s_enabled.load(std::memory_order_acquire);
	#else
			return true;
	#endif
		}
	}

	namespace memory_tracker
	{
		bool initialize() noexcept
		{
	#if EMBER_MEMORY_TRACKING >= 2
			if (s_enabled.load(std::memory_order_acquire))
				return true;

			reset_all_counters();
			s_next_allocation_id.store(1, std::memory_order_relaxed);
			s_break_allocation_id.store(0, std::memory_order_relaxed);

			{
				TrackerScope scope;
				std::lock_guard lock(s_allocation_mutex);
				if (s_allocations == nullptr)
					s_allocations = ::new (s_allocation_storage) AllocationMap();
				else
					s_allocations->clear();
			}

			s_enabled.store(true, std::memory_order_release);
	#endif
			return true;
		}

		void shutdown() noexcept
		{
	#if EMBER_MEMORY_TRACKING >= 2
			if (!s_enabled.exchange(false, std::memory_order_acq_rel))
				return;

			TrackerScope scope;
			std::lock_guard lock(s_allocation_mutex);
			if (s_allocations != nullptr)
			{
				s_allocations->~AllocationMap();
				s_allocations = nullptr;
			}
	#endif
		}

		void on_alloc(void* ptr, size_t size, MemoryTag tag, bool poison) noexcept
		{
			if (ptr == nullptr || !tracking_enabled() || t_in_tracker)
				return;

			EMBER_PROFILE_ALLOC(ptr, size);

			(void)increment_counters(s_total_counters, static_cast<u64>(size));

	#if EMBER_MEMORY_TRACKING >= 2
			tag							 = sanitize_tag(tag);
			const u64 previous_tag_bytes = increment_counters(s_tag_counters[tag_index(tag)], static_cast<u64>(size));
			maybe_warn_budget(tag, previous_tag_bytes, previous_tag_bytes + size);

			const u64 allocation_id = s_next_allocation_id.fetch_add(1, std::memory_order_relaxed);
			if (s_break_allocation_id.load(std::memory_order_relaxed) == allocation_id) [[unlikely]]
			{
				TrackerScope scope;
				Logger::warn(std::source_location::current(), "Break on allocation #{}", allocation_id);
				EMBER_DEBUG_BREAK();
			}

			TrackerScope scope;
			if (poison)
				std::memset(ptr, 0xCD, size);

			std::lock_guard lock(s_allocation_mutex);
			if (s_allocations != nullptr)
				(*s_allocations)[ptr] = AllocationRecord{static_cast<u64>(size), tag, allocation_id};
	#else
			(void)tag;
			(void)poison;
	#endif
		}

		void on_free(void* ptr, size_t size) noexcept
		{
			if (ptr == nullptr || !tracking_enabled() || t_in_tracker)
				return;

	#if EMBER_MEMORY_TRACKING >= 2
			(void)size; // The live map's record supplies the authoritative size at level 2.
			erase_allocation(ptr, true);
	#else
			EMBER_PROFILE_FREE(ptr);
			decrement_counters_saturating(s_total_counters, static_cast<u64>(size));
	#endif
		}

		void on_free_unpoisoned(void* ptr, size_t size) noexcept
		{
			if (ptr == nullptr || !tracking_enabled() || t_in_tracker)
				return;

	#if EMBER_MEMORY_TRACKING >= 2
			(void)size;
			erase_allocation(ptr, false);
	#else
			EMBER_PROFILE_FREE(ptr);
			decrement_counters_saturating(s_total_counters, static_cast<u64>(size));
	#endif
		}

		TagStats total() noexcept { return read_counters(s_total_counters); }

		void report(bool as_csv) noexcept
		{
			if (t_in_tracker)
				return;

			TrackerScope scope;

	#if EMBER_MEMORY_TRACKING >= 2
			if (as_csv)
			{
				Logger::info(
					std::source_location::current(), "tag,current_bytes,peak_bytes,current_count,total_count,budget");
				for (size_t i = 0; i < k_tag_count; ++i)
				{
					const auto tag	   = static_cast<MemoryTag>(i);
					const memory_tracker::TagStats row = memory_tracker::stats(tag);
					Logger::info(
						std::source_location::current(),
						"{},{},{},{},{},{}",
						tag_name(tag),
						row.current_bytes,
						row.peak_bytes,
						row.current_count,
						row.total_count,
						MEMORY_TAG_BUDGETS[i]);
				}
			}
			else
			{
				Logger::info(
					std::source_location::current(),
					"{:<12} {:>14} {:>14} {:>12} {:>12} {:>14}",
					"Tag",
					"Current",
					"Peak",
					"Live",
					"Total",
					"Budget");
				for (size_t i = 0; i < k_tag_count; ++i)
				{
					const auto tag	   = static_cast<MemoryTag>(i);
					const memory_tracker::TagStats row = memory_tracker::stats(tag);
					Logger::info(
						std::source_location::current(),
						"{:<12} {:>14} {:>14} {:>12} {:>12} {:>14}",
						tag_name(tag),
						row.current_bytes,
						row.peak_bytes,
						row.current_count,
						row.total_count,
						MEMORY_TAG_BUDGETS[i]);
				}
			}
	#endif

			const memory_tracker::TagStats totals = total();
			if (as_csv)
				Logger::info(
					std::source_location::current(),
					"Total,{},{},{},{}",
					totals.current_bytes,
					totals.peak_bytes,
					totals.current_count,
					totals.total_count);
			else
				Logger::info(
					std::source_location::current(),
					"{:<12} {:>14} {:>14} {:>12} {:>12}",
					"Total",
					totals.current_bytes,
					totals.peak_bytes,
					totals.current_count,
					totals.total_count);

			const ArenaResource& arena = memory::frame_arena();
			if (as_csv)
				Logger::info(
					std::source_location::current(),
					"FrameArena,{},{},{},{}",
					arena.used(),
					arena.committed(),
					arena.peak(),
					arena.reserved());
			else
				Logger::info(
					std::source_location::current(),
					"FrameArena used={} committed={} peak={} reserved={}",
					arena.used(),
					arena.committed(),
					arena.peak(),
					arena.reserved());
		}

	#if EMBER_MEMORY_TRACKING >= 2
		memory_tracker::TagStats stats(MemoryTag tag) noexcept { return read_counters(s_tag_counters[tag_index(tag)]); }

		u64 report_leaks() noexcept
		{
			if (t_in_tracker)
				return 0;

			TrackerScope scope;

			// Snapshot under the lock, log outside it — never log while holding the allocation mutex.
			std::vector<AllocationRecord, UntrackedAllocator<AllocationRecord>> leaks;
			{
				std::lock_guard lock(s_allocation_mutex);

				if (s_allocations == nullptr)
					return 0;

				leaks.reserve(s_allocations->size());
				for (const auto& [ptr, record] : *s_allocations)
				{
					(void)ptr;
					leaks.push_back(record);
				}
			}

			for (const AllocationRecord& record : leaks)
				Logger::warn(
					std::source_location::current(),
					"[{}] {} bytes, allocation #{}",
					tag_name(record.tag),
					record.size,
					record.allocation_id);

			return static_cast<u64>(leaks.size());
		}

		void break_on_allocation(u64 allocation_id) noexcept
		{
			s_break_allocation_id.store(allocation_id, std::memory_order_relaxed);
		}
	#endif
	}
}
#endif
