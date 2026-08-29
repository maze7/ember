#include <ember/containers/pool.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <random>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	using ember::Handle;
	using ember::MemoryTag;
	using ember::Pool;
	using ember::u16;
	using ember::u32;
	using ember::u64;

	struct BasicTag;
	struct OtherTag;
	struct TextureTag;
	struct LifetimeTag;
	struct AliasTag;
	struct ResourceTag;
	struct CapacityTag;
	struct GenerationTag;
	struct RingTag;
	struct StressTag;
	struct NonDefaultColdTag;
	struct OverflowTag;

	using BasicPool	  = Pool<BasicTag, int>;
	using BasicHandle = Handle<BasicTag>;

	static_assert(std::is_same_v<BasicPool::Hot, int>);
	static_assert(BasicPool::MAX_CAPACITY == 1u << 16);

	static_assert(!std::is_copy_constructible_v<BasicPool>);
	static_assert(!std::is_copy_assignable_v<BasicPool>);
	static_assert(std::is_nothrow_move_constructible_v<BasicPool>);
	static_assert(std::is_nothrow_move_assignable_v<BasicPool>);

	static_assert(std::forward_iterator<BasicPool::Iterator>);
	static_assert(std::forward_iterator<BasicPool::ConstIterator>);

	static_assert(!std::is_same_v<Handle<BasicTag>, Handle<OtherTag>>);

	template <typename HandleT>
	[[nodiscard]]
	u64 handle_key(HandleT handle) noexcept
	{
		return static_cast<u64>(handle.to_bits());
	}

	struct TrackedValue
	{
		explicit TrackedValue(int new_value) : value(new_value), text(std::to_string(new_value))
		{
			++constructions;
			++live;
		}

		TrackedValue(const TrackedValue& other) : value(other.value), text(other.text)
		{
			++constructions;
			++copies;
			++live;
		}

		TrackedValue(TrackedValue&& other) noexcept : value(other.value), text(std::move(other.text))
		{
			other.value = -1;

			++constructions;
			++moves;
			++live;
		}

		~TrackedValue() noexcept
		{
			++destructions;
			--live;
		}

		TrackedValue& operator=(const TrackedValue&) = delete;
		TrackedValue& operator=(TrackedValue&&)		 = delete;

		static void reset() noexcept
		{
			constructions = 0;
			destructions  = 0;
			copies		  = 0;
			moves		  = 0;
			live		  = 0;
		}

		int value;
		std::string text;

		static inline int constructions = 0;
		static inline int destructions	= 0;
		static inline int copies		= 0;
		static inline int moves			= 0;
		static inline int live			= 0;
	};

	struct TextureHot
	{
		u32 backend_id = 0;
	};

	struct TextureCold
	{
		u32 width  = 0;
		u32 height = 0;
		std::string debug_name;
	};

	using TexturePool = Pool<TextureTag, TextureHot, TextureCold>;

	struct NonDefaultCold
	{
		NonDefaultCold() = delete;

		explicit NonDefaultCold(int new_value) noexcept : value(new_value) {}

		int value;
	};

	static_assert(!std::is_default_constructible_v<NonDefaultCold>);

	struct alignas(128) OverAlignedHot
	{
		u64 value = 0;
	};

	struct alignas(256) OverAlignedCold
	{
		u64 value = 0;
	};

	class CountingResource final : public std::pmr::memory_resource
	{
	public:
		struct Record
		{
			void* pointer	 = nullptr;
			size_t bytes	 = 0;
			size_t alignment = 0;
			bool active		 = false;
		};

		size_t allocation_count	  = 0;
		size_t deallocation_count = 0;
		size_t outstanding		  = 0;
		size_t max_alignment	  = 0;
		bool valid_deallocations  = true;

	private:
		void* do_allocate(size_t bytes, size_t alignment) override
		{
			void* pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);

			++allocation_count;
			++outstanding;

			max_alignment = std::max(max_alignment, alignment);

			if (m_record_count < m_records.size())
			{
				m_records[m_record_count++] = {
					.pointer   = pointer,
					.bytes	   = bytes,
					.alignment = alignment,
					.active	   = true,
				};
			}
			else
			{
				valid_deallocations = false;
			}

			return pointer;
		}

		void do_deallocate(void* pointer, size_t bytes, size_t alignment) override
		{
			Record* record = nullptr;

			for (size_t i = 0; i < m_record_count; ++i)
			{
				if (m_records[i].active && m_records[i].pointer == pointer)
				{
					record = &m_records[i];
					break;
				}
			}

			++deallocation_count;

			if (outstanding == 0)
				valid_deallocations = false;
			else
				--outstanding;

			if (record == nullptr)
			{
				valid_deallocations = false;

				std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);

				return;
			}

			if (record->bytes != bytes || record->alignment != alignment)
			{
				valid_deallocations = false;
			}

			record->active = false;

			// Use the authoritative values so a failed test does not invoke UB
			// in the upstream resource.
			std::pmr::new_delete_resource()->deallocate(pointer, record->bytes, record->alignment);
		}

		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

		std::array<Record, 32> m_records{};
		size_t m_record_count = 0;
	};

	TEST(Pool, DefaultStateRejectsEverything)
	{
		BasicPool values;

		EXPECT_TRUE(values.empty());
		EXPECT_EQ(values.size(), 0u);
		EXPECT_EQ(values.capacity(), 0u);
		EXPECT_EQ(values.begin(), values.end());

		EXPECT_TRUE(values.insert(1).is_null());
		EXPECT_TRUE(values.emplace(2).is_null());
		EXPECT_EQ(values.get(BasicHandle{}), nullptr);
		EXPECT_FALSE(values.erase(BasicHandle{}));
		EXPECT_TRUE(values.empty());
	}

	TEST(Pool, InitAndBasicRoundTrip)
	{
		BasicPool values;
		values.init(8);

		EXPECT_TRUE(values.empty());
		EXPECT_EQ(values.capacity(), 8u);

		int lvalue = 42;

		const auto first  = values.insert(lvalue);
		const auto second = values.insert(7);
		const auto third  = values.emplace(9);

		EXPECT_FALSE(first.is_null());
		EXPECT_EQ(values.size(), 3u);
		EXPECT_FALSE(values.empty());

		ASSERT_NE(values.get(first), nullptr);
		EXPECT_EQ(*values.get(first), 42);
		EXPECT_EQ(*values.get(second), 7);
		EXPECT_EQ(*values.get(third), 9);

		const BasicPool& const_values = values;

		ASSERT_NE(const_values.get(second), nullptr);
		EXPECT_EQ(*const_values.get(second), 7);
	}

	TEST(Pool, FirstAllocationsAscendFromZero)
	{
		// The GPU layer's fallbacks claim bindless slot 0 by being the first insert.
		BasicPool values;
		values.init(4);

		EXPECT_EQ(values.insert(1).index, 0u);
		EXPECT_EQ(values.insert(2).index, 1u);
		EXPECT_EQ(values.insert(3).index, 2u);
		EXPECT_EQ(values.insert(4).index, 3u);
	}

	TEST(Pool, RejectsNullStaleWrongGenerationAndOutOfRangeHandles)
	{
		BasicPool values;
		values.init(4);

		const BasicHandle null_handle{};

		EXPECT_TRUE(null_handle.is_null());
		EXPECT_FALSE(values.contains(null_handle));
		EXPECT_EQ(values.get(null_handle), nullptr);
		EXPECT_FALSE(values.erase(null_handle));

		const auto live = values.insert(11);

		auto wrong_generation = live;
		++wrong_generation.generation;

		if (wrong_generation.generation == 0)
			wrong_generation.generation = 1;

		EXPECT_FALSE(values.contains(wrong_generation));
		EXPECT_EQ(values.get(wrong_generation), nullptr);
		EXPECT_FALSE(values.erase(wrong_generation));

		const BasicHandle out_of_range{
			static_cast<u16>(values.capacity()),
			1,
		};

		EXPECT_FALSE(values.contains(out_of_range));
		EXPECT_EQ(values.get(out_of_range), nullptr);
		EXPECT_FALSE(values.erase(out_of_range));

		ASSERT_TRUE(values.erase(live));

		EXPECT_FALSE(values.contains(live));
		EXPECT_EQ(values.get(live), nullptr);
		EXPECT_FALSE(values.erase(live));
	}

	TEST(Pool, FullPoolReturnsNullAndRecoversAfterErase)
	{
		BasicPool values;
		values.init(2);

		const auto first  = values.insert(1);
		const auto second = values.insert(2);

		ASSERT_FALSE(first.is_null());
		ASSERT_FALSE(second.is_null());

		const auto refused = values.insert(3);

		EXPECT_TRUE(refused.is_null());
		EXPECT_EQ(values.size(), 2u);

		ASSERT_TRUE(values.erase(first));

		const auto recovered = values.insert(4);

		EXPECT_FALSE(recovered.is_null());
		EXPECT_EQ(recovered.index, first.index);
		EXPECT_NE(recovered.generation, first.generation);
		EXPECT_EQ(*values.get(recovered), 4);
	}

	TEST(Pool, EraseDoesNotMoveSurvivingValues)
	{
		BasicPool values;
		values.init(16);

		const auto first  = values.insert(10);
		const auto middle = values.insert(20);
		const auto last	  = values.insert(30);

		int* first_address = values.get(first);
		int* last_address  = values.get(last);

		ASSERT_NE(first_address, nullptr);
		ASSERT_NE(last_address, nullptr);

		ASSERT_TRUE(values.erase(middle));

		EXPECT_EQ(values.size(), 2u);
		EXPECT_EQ(values.get(first), first_address);
		EXPECT_EQ(values.get(last), last_address);
		EXPECT_EQ(*values.get(first), 10);
		EXPECT_EQ(*values.get(last), 30);
	}

	TEST(Pool, ReusesErasedSlotsWithNewGenerations)
	{
		BasicPool values;
		values.init(1);

		const auto original = values.insert(11);

		ASSERT_TRUE(values.erase(original));

		const auto replacement = values.insert(22);

		EXPECT_EQ(replacement.index, original.index);
		EXPECT_NE(replacement.generation, original.generation);

		EXPECT_FALSE(values.contains(original));
		EXPECT_TRUE(values.contains(replacement));
		EXPECT_EQ(*values.get(replacement), 22);
	}

	TEST(Pool, FreeRingIsFifo)
	{
		BasicPool values;
		values.init(3);

		const auto first  = values.insert(1);
		const auto second = values.insert(2);
		const auto third  = values.insert(3);

		EXPECT_EQ(first.index, 0u);
		EXPECT_EQ(second.index, 1u);
		EXPECT_EQ(third.index, 2u);

		ASSERT_TRUE(values.erase(first));
		ASSERT_TRUE(values.erase(third));

		// Oldest free slot first.
		EXPECT_EQ(values.insert(4).index, first.index);
		EXPECT_EQ(values.insert(5).index, third.index);
		EXPECT_TRUE(values.insert(6).is_null());
	}

	TEST(Pool, FifoCyclesThroughVirginSlotsBeforeRecycledOnes)
	{
		BasicPool values;
		values.init(4);

		const auto a = values.insert(1);
		(void)values.insert(2);

		ASSERT_TRUE(values.erase(a));

		// Slots 2 and 3 were queued at init and are older than the freed slot 0.
		EXPECT_EQ(values.insert(3).index, 2u);
		EXPECT_EQ(values.insert(4).index, 3u);
		EXPECT_EQ(values.insert(5).index, a.index);
	}

	TEST(Pool, RingSeamWrapsCleanly)
	{
		Pool<RingTag, int> values;
		values.init(3);

		auto a = values.insert(0);
		auto b = values.insert(1);
		auto c = values.insert(2);

		ASSERT_FALSE(a.is_null() || b.is_null() || c.is_null());

		// One-in-one-out on a full pool reuses the just-freed slot every cycle,
		// walking the ring head across the seam repeatedly.
		auto oldest = a;

		for (int cycle = 0; cycle < 10; ++cycle)
		{
			const u16 freed_index = oldest.index;

			ASSERT_TRUE(values.erase(oldest));

			const auto replacement = values.insert(100 + cycle);

			ASSERT_FALSE(replacement.is_null());
			EXPECT_EQ(replacement.index, freed_index);
			EXPECT_EQ(*values.get(replacement), 100 + cycle);
			EXPECT_EQ(values.size(), 3u);

			oldest = cycle % 3 == 0 ? b : (cycle % 3 == 1 ? c : replacement);

			if (cycle % 3 == 0)
				b = replacement;
			else if (cycle % 3 == 1)
				c = replacement;
		}
	}

	TEST(Pool, ClearInvalidatesHandlesAndRetainsAllocation)
	{
		BasicPool values;
		values.init(8);

		const auto first		  = values.insert(1);
		const auto second		  = values.insert(2);
		const u32 capacity_before = values.capacity();

		values.clear();

		EXPECT_TRUE(values.empty());
		EXPECT_EQ(values.capacity(), capacity_before);
		EXPECT_FALSE(values.contains(first));
		EXPECT_FALSE(values.contains(second));
		EXPECT_EQ(values.begin(), values.end());

		const auto replacement = values.insert(3);

		EXPECT_EQ(replacement.index, 0u);
		EXPECT_NE(replacement.generation, first.generation);
		EXPECT_EQ(*values.get(replacement), 3);
		EXPECT_EQ(values.capacity(), capacity_before);

		values.clear();
		values.clear();

		EXPECT_TRUE(values.empty());
		EXPECT_EQ(values.capacity(), capacity_before);
	}

	TEST(Pool, IterationSkipsHolesAndProducesMatchingHandles)
	{
		BasicPool values;
		values.init(8);

		const auto erased_first	 = values.insert(10);
		const auto first_live	 = values.insert(20);
		const auto erased_middle = values.insert(30);
		const auto second_live	 = values.insert(40);
		const auto erased_last	 = values.insert(50);

		ASSERT_TRUE(values.erase(erased_first));
		ASSERT_TRUE(values.erase(erased_middle));
		ASSERT_TRUE(values.erase(erased_last));

		std::vector<int> visited(values.begin(), values.end());

		EXPECT_EQ(visited, (std::vector<int>{20, 40}));

		u32 visited_count = 0;

		for (auto iterator = values.begin(); iterator != values.end(); ++iterator)
		{
			const auto handle = iterator.handle();

			EXPECT_TRUE(values.contains(handle));
			EXPECT_EQ(values.get(handle), &*iterator);

			*iterator += 1;
			++visited_count;
		}

		EXPECT_EQ(visited_count, values.size());
		EXPECT_EQ(*values.get(first_live), 21);
		EXPECT_EQ(*values.get(second_live), 41);

		auto iterator = values.begin();
		auto previous = iterator++;

		EXPECT_EQ(*previous, 21);
		EXPECT_EQ(*iterator, 41);

		const BasicPool& const_values = values;

		EXPECT_EQ(std::count(const_values.begin(), const_values.end(), 41), 1);
	}

	TEST(Pool, AliasedInsertCopiesFromPoolStorageAndFullInsertConstructsNothing)
	{
		TrackedValue::reset();

		{
			Pool<AliasTag, TrackedValue> values;
			values.init(16);

			std::vector<Handle<AliasTag>> handles;

			for (int value = 0; value < 15; ++value)
				handles.push_back(values.emplace(value));

			const auto source = handles.front();

			// Copying out of pool storage into the last free slot.
			const auto copied = values.insert(*values.get(source));

			ASSERT_FALSE(copied.is_null());
			ASSERT_EQ(values.size(), values.capacity());

			EXPECT_EQ(values.get(source)->value, 0);
			EXPECT_EQ(values.get(source)->text, "0");
			EXPECT_EQ(values.get(copied)->value, 0);
			EXPECT_EQ(values.get(copied)->text, "0");
			EXPECT_GE(TrackedValue::copies, 1);

			// A refused insert runs no constructor and no destructor.
			const int constructions_before = TrackedValue::constructions;
			const int live_before		   = TrackedValue::live;

			const auto refused = values.insert(*values.get(source));

			EXPECT_TRUE(refused.is_null());
			EXPECT_EQ(TrackedValue::constructions, constructions_before);
			EXPECT_EQ(TrackedValue::live, live_before);
		}

		EXPECT_EQ(TrackedValue::live, 0);
		EXPECT_EQ(TrackedValue::constructions, TrackedValue::destructions);
	}

	TEST(Pool, FixedStorageNeverMovesValues)
	{
		TrackedValue::reset();

		{
			Pool<LifetimeTag, TrackedValue> values;
			values.init(16);

			std::vector<Handle<LifetimeTag>> handles;

			for (int value = 0; value < 16; ++value)
				handles.push_back(values.emplace(value));

			for (u32 index = 0; index < handles.size(); index += 2)
				ASSERT_TRUE(values.erase(handles[index]));

			for (u32 index = 0; index < handles.size(); ++index)
			{
				if (index % 2 == 0)
					EXPECT_FALSE(values.contains(handles[index]));
				else
					EXPECT_EQ(values.get(handles[index])->value, static_cast<int>(index));
			}

			values.clear();
			EXPECT_EQ(TrackedValue::live, 0);

			const auto reused = values.emplace(99);

			EXPECT_EQ(values.get(reused)->text, "99");
			EXPECT_EQ(TrackedValue::live, 1);

			EXPECT_EQ(TrackedValue::moves, 0);
			EXPECT_EQ(TrackedValue::copies, 0);
		}

		EXPECT_EQ(TrackedValue::live, 0);
		EXPECT_EQ(TrackedValue::constructions, TrackedValue::destructions);
	}

	TEST(Pool, HotAndColdStreamsShareOneHandle)
	{
		TexturePool textures;
		textures.init(8);

		const auto albedo = textures.insert(TextureHot{7}, TextureCold{16, 32, "albedo"});

		const auto normal = textures.emplace(TextureHot{9});

		EXPECT_EQ(textures.get(albedo)->backend_id, 7u);
		EXPECT_EQ(textures.get_cold(albedo)->width, 16u);
		EXPECT_EQ(textures.get_cold(albedo)->height, 32u);
		EXPECT_EQ(textures.get_cold(albedo)->debug_name, "albedo");

		// emplace() value-initializes the cold stream.
		EXPECT_EQ(textures.get(normal)->backend_id, 9u);
		EXPECT_EQ(textures.get_cold(normal)->width, 0u);
		EXPECT_EQ(textures.get_cold(normal)->height, 0u);
		EXPECT_TRUE(textures.get_cold(normal)->debug_name.empty());

		textures.get_cold(normal)->debug_name = "normal";

		const TexturePool& const_textures = textures;

		ASSERT_NE(const_textures.get_cold(normal), nullptr);

		EXPECT_EQ(const_textures.get_cold(normal)->debug_name, "normal");

		ASSERT_TRUE(textures.erase(albedo));

		EXPECT_EQ(textures.get(albedo), nullptr);
		EXPECT_EQ(textures.get_cold(albedo), nullptr);
	}

	TEST(Pool, HotAndColdAliasingIsSafeAtFullCapacity)
	{
		TexturePool textures;
		textures.init(17);

		std::vector<Handle<TextureTag>> handles;

		for (u32 index = 0; index < 16; ++index)
		{
			handles.push_back(textures.insert(
				TextureHot{index},
				TextureCold{
					index + 1,
					index + 2,
					std::to_string(index),
				}));
		}

		const auto source = handles.front();

		const auto copy = textures.insert(*textures.get(source), *textures.get_cold(source));

		ASSERT_FALSE(copy.is_null());
		ASSERT_EQ(textures.size(), textures.capacity());

		EXPECT_EQ(textures.get(source)->backend_id, 0u);
		EXPECT_EQ(textures.get_cold(source)->debug_name, "0");

		EXPECT_EQ(textures.get(copy)->backend_id, 0u);
		EXPECT_EQ(textures.get_cold(copy)->width, 1u);
		EXPECT_EQ(textures.get_cold(copy)->height, 2u);
		EXPECT_EQ(textures.get_cold(copy)->debug_name, "0");
	}

	TEST(Pool, ExplicitColdInsertionSupportsNonDefaultColdTypes)
	{
		Pool<NonDefaultColdTag, int, NonDefaultCold> values;
		values.init(1);

		const auto handle = values.insert(12, NonDefaultCold{34});

		EXPECT_EQ(*values.get(handle), 12);
		EXPECT_EQ(values.get_cold(handle)->value, 34);
	}

	TEST(Pool, MovedFromPoolIsEmptyAndCanBeReinitialized)
	{
		BasicPool source;
		source.init(8);

		const auto first  = source.insert(101);
		const auto second = source.insert(202);

		BasicPool moved(std::move(source));

		EXPECT_TRUE(source.empty());
		EXPECT_EQ(source.capacity(), 0u);
		EXPECT_EQ(*moved.get(first), 101);
		EXPECT_EQ(*moved.get(second), 202);

		// A moved-from pool has no storage; inserts refuse until init runs again.
		EXPECT_TRUE(source.insert(303).is_null());

		source.init(4);

		const auto reused_source = source.insert(303);

		EXPECT_EQ(*source.get(reused_source), 303);

		BasicPool assigned;
		assigned.init(4);
		(void)assigned.insert(-1);

		assigned = std::move(moved);

		EXPECT_TRUE(moved.empty());
		EXPECT_EQ(moved.capacity(), 0u);
		EXPECT_EQ(*assigned.get(first), 101);
		EXPECT_EQ(*assigned.get(second), 202);

		BasicPool& self = assigned;
		assigned		= std::move(self);

		EXPECT_EQ(*assigned.get(first), 101);
		EXPECT_EQ(*assigned.get(second), 202);
	}

	TEST(Pool, UsesOneAlignedPmrBlockAndBalancedDeallocation)
	{
		CountingResource resource;

		{
			Pool<ResourceTag, OverAlignedHot, OverAlignedCold> values(resource, MemoryTag::Graphics);

			values.init(17);

			EXPECT_EQ(resource.allocation_count, 1u);
			EXPECT_EQ(resource.deallocation_count, 0u);
			EXPECT_EQ(resource.outstanding, 1u);
			EXPECT_GE(resource.max_alignment, alignof(OverAlignedCold));

			const size_t allocations_before_insert = resource.allocation_count;

			const auto handle = values.insert(OverAlignedHot{11}, OverAlignedCold{22});

			EXPECT_EQ(resource.allocation_count, allocations_before_insert);

			ASSERT_NE(values.get(handle), nullptr);
			ASSERT_NE(values.get_cold(handle), nullptr);

			EXPECT_TRUE(ember::is_aligned(values.get(handle), alignof(OverAlignedHot)));
			EXPECT_TRUE(ember::is_aligned(values.get_cold(handle), alignof(OverAlignedCold)));

			EXPECT_EQ(values.get(handle)->value, 11u);
			EXPECT_EQ(values.get_cold(handle)->value, 22u);
		}

		EXPECT_EQ(resource.allocation_count, resource.deallocation_count);

		EXPECT_EQ(resource.outstanding, 0u);
		EXPECT_TRUE(resource.valid_deallocations);
	}

	TEST(Pool, MoveAssignmentTransfersTheSourceMemoryResource)
	{
		CountingResource source_resource;
		CountingResource destination_resource;

		{
			Pool<ResourceTag, int> source(source_resource, MemoryTag::Graphics);
			source.init(4);

			Pool<ResourceTag, int> destination(destination_resource, MemoryTag::Audio);
			destination.init(4);

			const auto source_handle = source.insert(123);
			(void)destination.insert(456);

			EXPECT_EQ(source_resource.outstanding, 1u);
			EXPECT_EQ(destination_resource.outstanding, 1u);

			destination = std::move(source);

			EXPECT_TRUE(source.empty());
			EXPECT_EQ(source.capacity(), 0u);
			EXPECT_EQ(*destination.get(source_handle), 123);

			// Destination's previous allocation was released through its
			// original resource.
			EXPECT_EQ(destination_resource.outstanding, 0u);
			EXPECT_EQ(source_resource.outstanding, 1u);
		}

		EXPECT_EQ(source_resource.outstanding, 0u);
		EXPECT_EQ(destination_resource.outstanding, 0u);
		EXPECT_TRUE(source_resource.valid_deallocations);
		EXPECT_TRUE(destination_resource.valid_deallocations);
	}

	TEST(Pool, FillsEntireRepresentableIndexSpace)
	{
		constexpr u32 maximum = Pool<CapacityTag, u32>::MAX_CAPACITY;

		Pool<CapacityTag, u32> values;
		values.init(maximum);

		EXPECT_EQ(values.capacity(), maximum);

		const auto first = values.insert(0u);
		auto last		 = first;

		for (u32 index = 1; index < maximum; ++index)
			last = values.insert(index);

		EXPECT_EQ(values.size(), maximum);
		EXPECT_EQ(first.index, 0u);
		EXPECT_EQ(last.index, maximum - 1);
		EXPECT_EQ(*values.get(first), 0u);
		EXPECT_EQ(*values.get(last), maximum - 1);

		EXPECT_TRUE(values.insert(0u).is_null());
	}

	TEST(Pool, GenerationWrapBoundIsExplicit)
	{
		constexpr u32 maximum_generation = std::numeric_limits<u16>::max();

		Pool<GenerationTag, int> values;
		values.init(1);

		const auto original = values.insert(1);

		ASSERT_TRUE(values.erase(original));

		// Generations 2...65535 remain distinct from generation 1.
		for (u32 cycle = 0; cycle < maximum_generation - 1; ++cycle)
		{
			const auto current = values.insert(static_cast<int>(cycle));

			ASSERT_EQ(current.index, original.index);
			ASSERT_NE(current.generation, original.generation);

			ASSERT_TRUE(values.erase(current));
		}

		// A finite generation field must eventually alias. Engine code must
		// not retain stale handles indefinitely across this many recycles.
		const auto wrapped = values.insert(999);

		EXPECT_EQ(wrapped.index, original.index);
		EXPECT_EQ(wrapped.generation, original.generation);

		EXPECT_TRUE(values.contains(original));
		EXPECT_EQ(*values.get(original), 999);
	}

	TEST(Pool, ForgedHandleCannotCorruptTheFreeRing)
	{
		BasicPool values;
		values.init(2);

		const auto live = values.insert(1);

		// Wrong generation against a live slot fails on the compare.
		auto forged = live;
		++forged.generation;

		if (forged.generation == 0)
			forged.generation = 1;

		EXPECT_FALSE(values.erase(forged));

		ASSERT_TRUE(values.erase(live));

		// The freed slot now holds the forged generation; the live test is what
		// keeps this erase from double-pushing the index.
		EXPECT_FALSE(values.erase(forged));
		EXPECT_FALSE(values.contains(forged));

		// Exactly two slots remain allocatable: the ring was not corrupted.
		EXPECT_FALSE(values.insert(2).is_null());
		EXPECT_FALSE(values.insert(3).is_null());
		EXPECT_TRUE(values.insert(4).is_null());
	}

	TEST(Pool, RandomizedOperationsMatchReferenceModel)
	{
		constexpr int operation_count  = 50'000;
		constexpr size_t maximum_live  = 2'048;
		constexpr u32 maximum_capacity = 4'096;

		using StressPool   = Pool<StressTag, int>;
		using StressHandle = Handle<StressTag>;

		StressPool values;
		values.init(maximum_capacity);

		std::vector<StressHandle> live_handles;
		std::vector<StressHandle> stale_handles;
		std::unordered_map<u64, int> reference;

		std::mt19937 random(0x5EED1234u);

		auto validate = [&]
		{
			ASSERT_EQ(values.size(), live_handles.size());
			ASSERT_EQ(values.size(), reference.size());

			u32 walked = 0;

			for (auto iterator = values.begin(); iterator != values.end(); ++iterator)
			{
				const auto found = reference.find(handle_key(iterator.handle()));

				ASSERT_NE(found, reference.end());
				EXPECT_EQ(found->second, *iterator);
				++walked;
			}

			EXPECT_EQ(walked, values.size());

			for (const StressHandle handle : live_handles)
			{
				const auto found = reference.find(handle_key(handle));

				ASSERT_NE(found, reference.end());
				ASSERT_NE(values.get(handle), nullptr);
				EXPECT_EQ(*values.get(handle), found->second);
			}

			for (const StressHandle handle : stale_handles)
			{
				EXPECT_FALSE(values.contains(handle));
				EXPECT_EQ(values.get(handle), nullptr);
			}
		};

		for (int operation = 0; operation < operation_count; ++operation)
		{
			const u32 choice = random() % 100;

			if (live_handles.empty() || (choice < 55 && live_handles.size() < maximum_live))
			{
				const int value			  = operation * 17 + 3;
				const StressHandle handle = values.insert(value);

				ASSERT_FALSE(handle.is_null());

				live_handles.push_back(handle);

				ASSERT_TRUE(reference.emplace(handle_key(handle), value).second);
			}
			else if (choice < 85)
			{
				const size_t index = static_cast<size_t>(random()) % live_handles.size();

				const StressHandle handle = live_handles[index];

				ASSERT_TRUE(values.erase(handle));
				ASSERT_EQ(reference.erase(handle_key(handle)), 1u);

				if (stale_handles.size() < 256)
					stale_handles.push_back(handle);

				live_handles[index] = live_handles.back();

				live_handles.pop_back();
			}
			else if (choice < 90)
			{
				if (!live_handles.empty() && stale_handles.size() < 256)
				{
					stale_handles.push_back(live_handles.back());
				}

				values.clear();
				live_handles.clear();
				reference.clear();
			}
			else
			{
				const size_t index = static_cast<size_t>(random()) % live_handles.size();

				const StressHandle handle = live_handles[index];

				const int replacement = -operation - 1;

				*values.get(handle)				 = replacement;
				reference.at(handle_key(handle)) = replacement;
			}

			if (operation % 257 == 0)
				validate();
		}

		validate();
	}

#ifndef NDEBUG
	TEST(PoolDeathTest, InitTwiceIsFatal)
	{
		EXPECT_DEATH(
			{
				BasicPool values;
				values.init(4);
				values.init(4);
			},
			"assert");
	}

	TEST(PoolDeathTest, InitWithZeroCapacityIsFatal)
	{
		EXPECT_DEATH(
			{
				BasicPool values;
				values.init(0);
			},
			"assert");
	}

	TEST(PoolDeathTest, InitAboveIndexLimitIsFatal)
	{
		using OverflowPool = Pool<OverflowTag, u32>;

		EXPECT_DEATH(
			{
				OverflowPool values;
				values.init(OverflowPool::MAX_CAPACITY + 1u);
			},
			"assert");
	}

	TEST(PoolDeathTest, GetTrapsForgedHandleToFreedSlot)
	{
		// A handle forged to the generation a freed slot currently holds passes
		// the compare; the debug assert on the live bit is what catches it.
		EXPECT_DEATH(
			{
				BasicPool values;
				values.init(1);

				const auto handle = values.insert(1);

				auto forged = handle;
				++forged.generation;

				if (forged.generation == 0)
					forged.generation = 1;

				(void)values.erase(handle);
				(void)values.get(forged);
			},
			"assert");
	}
#endif
}
