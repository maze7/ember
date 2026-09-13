#pragma once

#include <ember/memory/memory.h>

#include <memory>
#include <type_traits>
#include <utility>

namespace ember::memory
{
	template <typename T> struct HeapDelete final
	{
		void operator()(T* ptr) const noexcept
		{
			static_assert(!std::is_array_v<T>, "Unique<T> does not support array types");
			static_assert(std::is_nothrow_destructible_v<T>, "Heap-owned objects must have noexcept destructors");

			// All tagged HeapResources front the same process heap. Allocation
			// attribution is retained by the memory tracker, so the tag does not
			// need to travel with the pointer.
			delete_object(memory::heap(), ptr);
		}
	};
}

namespace ember
{
	template <typename T> using Unique = std::unique_ptr<T, memory::HeapDelete<T>>;
}

namespace ember::memory
{
	template <typename T, typename... Args>
	[[nodiscard]] Unique<T> make_unique(HeapResource& resource, Args&&... args) noexcept
	{
		static_assert(!std::is_array_v<T>, "Use PMR container for dynamic arrays");
		static_assert(std::is_nothrow_constructible_v<T, Args&&...>,
					  "Fallible construction belongs in T::create(), not a constructor");
		static_assert(std::is_nothrow_destructible_v<T>, "Heap-owned objects must have noexcept destructors");

		return Unique<T>{new_object<T>(resource, std::forward<Args>(args)...)};
	}

	template <typename T, typename... Args> [[nodiscard]] Unique<T> make_unique(MemoryTag tag, Args&&... args) noexcept
	{
		return make_unique<T>(heap(tag), std::forward<Args>(args)...);
	}
}
