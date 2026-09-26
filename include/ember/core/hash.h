#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>

namespace ember
{
	/**
	 * FNV-1a, the engine's content hash: cache keys, asset ids, anything written to disk and
	 * compared later. Small, constexpr and identical on every platform. Not cryptographic and not
	 * for hash tables over adversarial input; ankerl's hash does those.
	 */
	inline constexpr u64 HASH_SEED = 0xcbf29ce484222325ull;

	[[nodiscard]] constexpr u64 hash_bytes(Span<const u8> bytes, u64 hash = HASH_SEED) noexcept
	{
		for (size_t i = 0; i < bytes.size(); ++i)
			hash = (hash ^ bytes[i]) * 0x100000001b3ull;

		return hash;
	}

	[[nodiscard]] constexpr u64 hash_text(StringView text, u64 hash = HASH_SEED) noexcept
	{
		for (const char c : text)
			hash = (hash ^ static_cast<u8>(c)) * 0x100000001b3ull;

		return hash;
	}

	/// Folds a value's bytes into a running hash: hash_value(x, hash_value(y)) chains.
	template <class T> [[nodiscard]] u64 hash_value(const T& value, u64 hash = HASH_SEED) noexcept
	{
		static_assert(std::is_trivially_copyable_v<T>, "hash the bytes of plain data only");
		return hash_bytes({reinterpret_cast<const u8*>(&value), sizeof(T)}, hash);
	}
}
