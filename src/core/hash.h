#pragma once

#include <functional>
#include "core/common.h"

namespace Ember
{
	inline void hash_combine(u64& seed, u64 v) {
		// widely used hash combination ripped from Boost
		seed ^= v + 0x9e3779b97f4a7c16ULL + (seed << 6) + (seed >> 2);
	}

	template <class T>
	inline void hash_value(u64& seed, const T& val) {
		std::hash<T> hasher;
		hash_combine(seed, hasher(val));
	}

	template <typename... Ts>
	u64 combined_hash(const Ts&... vals) {
		u64 seed = 0;
		(hash_value(seed, vals), ...); // c++20 fold expression
		return seed;
	}
}
