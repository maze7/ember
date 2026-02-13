#pragma once

#include "core/common.h"
#include "core/hash.h"

namespace Ember
{
	template<class T, class IndexType = u32>
	struct Handle
	{
		/// value_type is the type of the data the handle accesses within a Pool
		using value_type = T;
		/// index_type is the type used for the index / generation fields.
		using index_type = IndexType;

		/// Slot is the index of the handle in the Pool.
		index_type index{0};
		/// Generation (version) of the data at time of Handle creation.
		index_type generation{0};

		/// A static constant member representing a null handle.
        static const Handle null;

		/// Allows keys to be compared
		auto operator<=>(const Handle&) const = default;

		// Utility method to determine if this handle is null
		[[nodiscard]] bool is_null() const {
		    return *this == null;
		}
	};

	template <class T, class IndexType>
    const Handle<T, IndexType> Handle<T, IndexType>::null{
        std::numeric_limits<IndexType>::max(), 0
    };
}

namespace std
{
    template<class T, class IndexType>
    struct hash<Ember::Handle<T, IndexType>>
    {
        // The return type must be size_t for std::hash
        size_t operator()(const Ember::Handle<T, IndexType>& handle) const noexcept {
            return Ember::combined_hash(handle.index, handle.generation);
        }
    };
}
