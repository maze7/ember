#include <ember/containers/pool.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <concepts>
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
	struct StressTag;
	struct NonDefaultColdTag;
	struct OverflowTag;

	using BasicPool = Pool<BasicTag, int>;

	static_assert(std::is_same_v<BasicPool::HandleType, Handle<BasicTag, u16>>);

	static_assert(!std::is_copy_constructible_v<BasicPool>);
	static_assert(!std::is_copy_assignable_v<BasicPool>);
	static_assert(std::is_nothrow_move_constructible_v<BasicPool>);
	static_assert(std::is_nothrow_move_assignable_v<BasicPool>);

	static_assert(std::forward_iterator<BasicPool::Iterator>);
	static_assert(std::forward_iterator<BasicPool::ConstIterator>);

	static_assert(!std::is_same_v<BasicPool::HandleType, Pool<OtherTag, int>::HandleType>);

	template <typename HandleType>
	[[nodiscard]]
	u64 handle_key(HandleType handle) noexcept
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

	TEST(Pool, DefaultStateAndBasicRoundTrip)
	{
		BasicPool values;

		EXPECT_TRUE(values.empty());
		EXPECT_EQ(values.size(), 0u);
		EXPECT_EQ(values.capacity(), 0u);
		EXPECT_EQ(values.begin(), values.end());

		int lvalue = 42;

		const auto first  = values.insert(lvalue);
		const auto second = values.insert(7);
		const auto third  = values.emplace(9);

		EXPECT_FALSE(first.is_null());
		EXPECT_EQ(values.size(), 3u);
		EXPECT_FALSE(values.empty());

		EXPECT_EQ(values.get(first), 42);
		EXPECT_EQ(values.get(second), 7);
		EXPECT_EQ(values.get(third), 9);

		EXPECT_EQ(values.try_get(first), &values.get(first));

		const BasicPool& const_values = values;

		ASSERT_NE(const_values.try_get(second), nullptr);
		EXPECT_EQ(const_values.get(second), 7);
	}

	TEST(Pool, RejectsNullStaleWrongGenerationAndOutOfRangeHandles)
	{
		BasicPool values;

		const BasicPool::HandleType null_handle{};

		EXPECT_TRUE(null_handle.is_null());
		EXPECT_FALSE(values.contains(null_handle));
		EXPECT_EQ(values.try_get(null_handle), nullptr);
		EXPECT_FALSE(values.erase(null_handle));

		const auto live = values.insert(11);

		auto wrong_generation = live;
		++wrong_generation.generation;

		if (wrong_generation.generation == 0)
			wrong_generation.generation = 1;

		EXPECT_FALSE(values.contains(wrong_generation));
		EXPECT_EQ(values.try_get(wrong_generation), nullptr);
		EXPECT_FALSE(values.erase(wrong_generation));

		const BasicPool::HandleType out_of_range{
			static_cast<u16>(values.capacity()),
			1,
		};

		EXPECT_FALSE(values.contains(out_of_range));
		EXPECT_EQ(values.try_get(out_of_range), nullptr);
		EXPECT_FALSE(values.erase(out_of_range));

		ASSERT_TRUE(values.erase(live));

		EXPECT_FALSE(values.contains(live));
		EXPECT_EQ(values.try_get(live), nullptr);
		EXPECT_FALSE(values.erase(live));
	}

	TEST(Pool, EraseDoesNotMoveSurvivingValues)
	{
		BasicPool values;
		values.reserve(16);

		const auto first  = values.insert(10);
		const auto middle = values.insert(20);
		const auto last	  = values.insert(30);

		int* first_address = values.try_get(first);
		int* last_address  = values.try_get(last);

		ASSERT_NE(first_address, nullptr);
		ASSERT_NE(last_address, nullptr);

		ASSERT_TRUE(values.erase(middle));

		EXPECT_EQ(values.size(), 2u);
		EXPECT_EQ(values.try_get(first), first_address);
		EXPECT_EQ(values.try_get(last), last_address);
		EXPECT_EQ(values.get(first), 10);
		EXPECT_EQ(values.get(last), 30);
	}

	TEST(Pool, ReusesErasedSlotsWithNewGenerations)
	{
		BasicPool values;

		const auto original = values.insert(11);

		ASSERT_TRUE(values.erase(original));

		const auto replacement = values.insert(22);

		EXPECT_EQ(replacement.index, original.index);
		EXPECT_NE(replacement.generation, original.generation);

		EXPECT_FALSE(values.contains(original));
		EXPECT_TRUE(values.contains(replacement));
		EXPECT_EQ(values.get(replacement), 22);
	}

	TEST(Pool, FreeListIsLastFreedFirst)
	{
		BasicPool values;

		const auto first  = values.insert(1);
		const auto second = values.insert(2);
		const auto third  = values.insert(3);

		EXPECT_EQ(first.index, 0u);
		EXPECT_EQ(second.index, 1u);
		EXPECT_EQ(third.index, 2u);

		ASSERT_TRUE(values.erase(first));
		ASSERT_TRUE(values.erase(third));

		EXPECT_EQ(values.insert(4).index, third.index);
		EXPECT_EQ(values.insert(5).index, first.index);
		EXPECT_EQ(values.insert(6).index, 3u);
	}

	TEST(Pool, ExplicitGrowthUsesFreshSlotsBeforeRecycledSlots)
	{
		BasicPool values;
		values.reserve(16);

		const auto erased	= values.insert(1);
		const auto survivor = values.insert(2);

		ASSERT_TRUE(values.erase(erased));

		values.reserve(17);

		EXPECT_EQ(values.capacity(), 32u);
		EXPECT_EQ(values.get(survivor), 2);

		const auto first_fresh	= values.insert(3);
		const auto second_fresh = values.insert(4);

		EXPECT_EQ(first_fresh.index, 16u);
		EXPECT_EQ(second_fresh.index, 17u);
		EXPECT_FALSE(values.contains(erased));
	}

	TEST(Pool, ClearInvalidatesHandlesAndRetainsAllocation)
	{
		BasicPool values;

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
		EXPECT_EQ(values.get(replacement), 3);
		EXPECT_EQ(values.capacity(), capacity_before);

		values.clear();
		values.clear();

		EXPECT_TRUE(values.empty());
		EXPECT_EQ(values.capacity(), capacity_before);
	}

	TEST(Pool, IterationSkipsHolesAndProducesMatchingHandles)
	{
		BasicPool values;

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
			EXPECT_EQ(values.try_get(handle), &*iterator);

			*iterator += 1;
			++visited_count;
		}

		EXPECT_EQ(visited_count, values.size());
		EXPECT_EQ(values.get(first_live), 21);
		EXPECT_EQ(values.get(second_live), 41);

		auto iterator = values.begin();
		auto previous = iterator++;

		EXPECT_EQ(*previous, 21);
		EXPECT_EQ(*iterator, 41);

		const BasicPool& const_values = values;

		EXPECT_EQ(std::count(const_values.begin(), const_values.end(), 41), 1);
	}

	TEST(Pool, GrowthPreservesAllHandlesAndValues)
	{
		BasicPool values;
		std::vector<BasicPool::HandleType> handles;

		for (int value = 0; value < 1000; ++value)
			handles.push_back(values.insert(value * 3));

		EXPECT_GE(values.capacity(), 1000u);
		EXPECT_EQ(values.size(), 1000u);

		for (u32 i = 0; i < handles.size(); ++i)
		{
			ASSERT_TRUE(values.contains(handles[i]));
			EXPECT_EQ(values.get(handles[i]), static_cast<int>(i) * 3);
		}
	}

	TEST(Pool, ReservePreventsInsertionGrowthAndNeverShrinks)
	{
		BasicPool values;

		values.reserve(100);

		const u32 reserved_capacity = values.capacity();

		EXPECT_GE(reserved_capacity, 100u);

		for (int value = 0; value < 100; ++value)
		{
			const auto handle = values.insert(value);

			EXPECT_EQ(values.get(handle), value);
			EXPECT_EQ(values.capacity(), reserved_capacity);
		}

		values.reserve(1);
		values.reserve(reserved_capacity);

		EXPECT_EQ(values.capacity(), reserved_capacity);
	}

	TEST(Pool, GrowthMaterializesAliasedLvalueBeforeRelocation)
	{
		TrackedValue::reset();

		{
			Pool<AliasTag, TrackedValue> values;
			values.reserve(16);

			std::vector<Pool<AliasTag, TrackedValue>::HandleType> handles;

			for (int value = 0; value < 16; ++value)
				handles.push_back(values.emplace(value));

			ASSERT_EQ(values.size(), values.capacity());

			const auto source = handles.front();

			const auto copied = values.insert(values.get(source));

			EXPECT_EQ(values.get(source).value, 0);
			EXPECT_EQ(values.get(source).text, "0");
			EXPECT_EQ(values.get(copied).value, 0);
			EXPECT_EQ(values.get(copied).text, "0");
			EXPECT_GE(TrackedValue::copies, 1);
		}

		EXPECT_EQ(TrackedValue::live, 0);
		EXPECT_EQ(TrackedValue::constructions, TrackedValue::destructions);
	}

	TEST(Pool, NonTrivialRelocationMovesOnlyLiveSlots)
	{
		TrackedValue::reset();

		{
			Pool<LifetimeTag, TrackedValue> values;
			std::vector<Pool<LifetimeTag, TrackedValue>::HandleType> handles;

			values.reserve(16);

			for (int value = 0; value < 16; ++value)
				handles.push_back(values.emplace(value));

			for (u32 index = 0; index < handles.size(); index += 2)
				ASSERT_TRUE(values.erase(handles[index]));

			const int live_before  = TrackedValue::live;
			const int moves_before = TrackedValue::moves;

			values.reserve(64);

			EXPECT_EQ(TrackedValue::live, live_before);
			EXPECT_EQ(TrackedValue::moves - moves_before, live_before);

			for (u32 index = 0; index < handles.size(); ++index)
			{
				if (index % 2 == 0)
					EXPECT_FALSE(values.contains(handles[index]));
				else
					EXPECT_EQ(values.get(handles[index]).value, static_cast<int>(index));
			}

			values.clear();
			EXPECT_EQ(TrackedValue::live, 0);

			const auto reused = values.emplace(99);

			EXPECT_EQ(values.get(reused).text, "99");
			EXPECT_EQ(TrackedValue::live, 1);
		}

		EXPECT_EQ(TrackedValue::live, 0);
		EXPECT_EQ(TrackedValue::constructions, TrackedValue::destructions);
	}

	TEST(Pool, HotAndColdStreamsShareOneHandle)
	{
		TexturePool textures;

		const auto albedo = textures.insert(TextureHot{7}, TextureCold{16, 32, "albedo"});

		const auto normal = textures.emplace(TextureHot{9});

		EXPECT_EQ(textures.get(albedo).backend_id, 7u);
		EXPECT_EQ(textures.get_cold(albedo).width, 16u);
		EXPECT_EQ(textures.get_cold(albedo).height, 32u);
		EXPECT_EQ(textures.get_cold(albedo).debug_name, "albedo");

		// emplace() value-initializes the cold stream.
		EXPECT_EQ(textures.get(normal).backend_id, 9u);
		EXPECT_EQ(textures.get_cold(normal).width, 0u);
		EXPECT_EQ(textures.get_cold(normal).height, 0u);
		EXPECT_TRUE(textures.get_cold(normal).debug_name.empty());

		textures.get_cold(normal).debug_name = "normal";

		const TexturePool& const_textures = textures;

		ASSERT_NE(const_textures.try_get_cold(normal), nullptr);

		EXPECT_EQ(const_textures.get_cold(normal).debug_name, "normal");

		ASSERT_TRUE(textures.erase(albedo));

		EXPECT_EQ(textures.try_get(albedo), nullptr);
		EXPECT_EQ(textures.try_get_cold(albedo), nullptr);
	}

	TEST(Pool, HotAndColdAliasingIsSafeDuringGrowth)
	{
		TexturePool textures;
		textures.reserve(16);

		std::vector<TexturePool::HandleType> handles;

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

		ASSERT_EQ(textures.size(), textures.capacity());

		const auto source = handles.front();

		const auto copy = textures.insert(textures.get(source), textures.get_cold(source));

		EXPECT_EQ(textures.get(source).backend_id, 0u);
		EXPECT_EQ(textures.get_cold(source).debug_name, "0");

		EXPECT_EQ(textures.get(copy).backend_id, 0u);
		EXPECT_EQ(textures.get_cold(copy).width, 1u);
		EXPECT_EQ(textures.get_cold(copy).height, 2u);
		EXPECT_EQ(textures.get_cold(copy).debug_name, "0");
	}

	TEST(Pool, ExplicitColdInsertionSupportsNonDefaultColdTypes)
	{
		Pool<NonDefaultColdTag, int, NonDefaultCold> values;

		const auto handle = values.insert(12, NonDefaultCold{34});

		EXPECT_EQ(values.get(handle), 12);
		EXPECT_EQ(values.get_cold(handle).value, 34);
	}

	TEST(Pool, MoveConstructionAssignmentAndSelfMovePreserveStorage)
	{
		BasicPool source;

		const auto first  = source.insert(101);
		const auto second = source.insert(202);

		BasicPool moved(std::move(source));

		EXPECT_TRUE(source.empty());
		EXPECT_EQ(source.capacity(), 0u);
		EXPECT_EQ(moved.get(first), 101);
		EXPECT_EQ(moved.get(second), 202);

		BasicPool assigned;
		(void)assigned.insert(-1);

		assigned = std::move(moved);

		EXPECT_TRUE(moved.empty());
		EXPECT_EQ(moved.capacity(), 0u);
		EXPECT_EQ(assigned.get(first), 101);
		EXPECT_EQ(assigned.get(second), 202);

		BasicPool& self = assigned;
		assigned = std::move(self);

		EXPECT_EQ(assigned.get(first), 101);
		EXPECT_EQ(assigned.get(second), 202);

		const auto reused_source = source.insert(303);

		EXPECT_EQ(source.get(reused_source), 303);
	}

	TEST(Pool, UsesOneAlignedPmrBlockAndBalancedDeallocation)
	{
		CountingResource resource;

		{
			Pool<ResourceTag, OverAlignedHot, OverAlignedCold> values(resource, MemoryTag::Graphics);

			values.reserve(17);

			EXPECT_EQ(resource.allocation_count, 1u);
			EXPECT_EQ(resource.deallocation_count, 0u);
			EXPECT_EQ(resource.outstanding, 1u);
			EXPECT_GE(resource.max_alignment, alignof(OverAlignedCold));

			const size_t allocations_before_insert = resource.allocation_count;

			const auto handle = values.insert(OverAlignedHot{11}, OverAlignedCold{22});

			EXPECT_EQ(resource.allocation_count, allocations_before_insert);

			ASSERT_NE(values.try_get(handle), nullptr);
			ASSERT_NE(values.try_get_cold(handle), nullptr);

			EXPECT_TRUE(ember::is_aligned(values.try_get(handle), alignof(OverAlignedHot)));

			EXPECT_TRUE(ember::is_aligned(values.try_get_cold(handle), alignof(OverAlignedCold)));

			values.reserve(65);

			EXPECT_EQ(resource.allocation_count, 2u);
			EXPECT_EQ(resource.deallocation_count, 1u);
			EXPECT_EQ(resource.outstanding, 1u);
			EXPECT_EQ(values.get(handle).value, 11u);
			EXPECT_EQ(values.get_cold(handle).value, 22u);
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

			Pool<ResourceTag, int> destination(destination_resource, MemoryTag::Audio);

			const auto source_handle = source.insert(123);
			(void)destination.insert(456);

			EXPECT_EQ(source_resource.outstanding, 1u);
			EXPECT_EQ(destination_resource.outstanding, 1u);

			destination = std::move(source);

			EXPECT_TRUE(source.empty());
			EXPECT_EQ(source.capacity(), 0u);
			EXPECT_EQ(destination.get(source_handle), 123);

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
		constexpr u32 maximum = std::numeric_limits<u16>::max();

		Pool<CapacityTag, u32> values;
		values.reserve(maximum);

		EXPECT_EQ(values.capacity(), maximum);

		const auto first = values.insert(0u);
		auto last		 = first;

		for (u32 index = 1; index < maximum; ++index)
			last = values.insert(index);

		EXPECT_EQ(values.size(), maximum);
		EXPECT_EQ(first.index, 0u);
		EXPECT_EQ(last.index, maximum - 1);
		EXPECT_EQ(values.get(first), 0u);
		EXPECT_EQ(values.get(last), maximum - 1);
	}

	TEST(Pool, GenerationWrapBoundIsExplicit)
	{
		constexpr u32 maximum_generation = std::numeric_limits<u16>::max();

		Pool<GenerationTag, int> values;

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
		EXPECT_EQ(values.get(original), 999);
	}

	TEST(Pool, RandomizedOperationsMatchReferenceModel)
	{
		constexpr int operation_count  = 50'000;
		constexpr size_t maximum_live  = 2'048;
		constexpr u32 maximum_capacity = 4'096;

		using StressPool   = Pool<StressTag, int>;
		using StressHandle = StressPool::HandleType;

		StressPool values;
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
				ASSERT_NE(values.try_get(handle), nullptr);
				EXPECT_EQ(values.get(handle), found->second);
			}

			for (const StressHandle handle : stale_handles)
			{
				EXPECT_FALSE(values.contains(handle));
				EXPECT_EQ(values.try_get(handle), nullptr);
			}
		};

		for (int operation = 0; operation < operation_count; ++operation)
		{
			const u32 choice = random() % 100;

			if (live_handles.empty() || (choice < 55 && live_handles.size() < maximum_live))
			{
				const int value			  = operation * 17 + 3;
				const StressHandle handle = values.insert(value);

				live_handles.push_back(handle);

				ASSERT_TRUE(reference.emplace(handle_key(handle), value).second);
			}
			else if (choice < 82)
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
			else if (choice < 88)
			{
				if (values.capacity() < maximum_capacity)
				{
					const u32 requested =
						std::min(maximum_capacity, values.capacity() + 1u + static_cast<u32>(random() % 256u));

					values.reserve(requested);
				}
			}
			else if (choice < 93)
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

				values.get(handle)				 = replacement;
				reference.at(handle_key(handle)) = replacement;
			}

			if (operation % 257 == 0)
				validate();
		}

		validate();
	}

	TEST(PoolDeathTest, CapacityAboveIndexLimitIsFatal)
	{
		using OverflowPool = Pool<OverflowTag, u32>;

		constexpr u32 invalid_capacity = static_cast<u32>(std::numeric_limits<u16>::max()) + 1u;

		EXPECT_DEATH(
			{
				OverflowPool values;
				values.reserve(invalid_capacity);
			},
			"Out of memory");
	}

#ifndef NDEBUG
	TEST(PoolDeathTest, GetRejectsNullHandle)
	{
		EXPECT_DEATH(
			{
				BasicPool values;
				(void)values.get({});
			},
			"assert");
	}

	TEST(PoolDeathTest, GetRejectsStaleHandle)
	{
		EXPECT_DEATH(
			{
				BasicPool values;
				const auto handle = values.insert(1);
				(void)values.erase(handle);
				(void)values.get(handle);
			},
			"assert");
	}

	TEST(PoolDeathTest, GetColdRejectsStaleHandle)
	{
		EXPECT_DEATH(
			{
				TexturePool values;

				const auto handle = values.insert(TextureHot{1}, TextureCold{1, 1, "stale"});

				(void)values.erase(handle);
				(void)values.get_cold(handle);
			},
			"assert");
	}
#endif
}
