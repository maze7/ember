#pragma once

#include <ember/core/common.h>

namespace ember
{
	/**
	 * Names an asset: FNV-1a of its path relative to the asset root. Backslashes hash as
	 * forward slashes so the same file names the same asset whichever platform wrote the
	 * path, and the file watcher can name an asset from path alone. constexpr, so a literal
	 * path is a constant.
	 */
	using AssetId = u64;

	[[nodiscard]] constexpr AssetId asset_id(StringView path) noexcept
	{
		u64 hash = 0xcbf29ce484222325ull;

		for (const char c : path)
		{
			const u8 byte = c == '\\' ? u8{'/'} : static_cast<u8>(c);
			hash		  = (hash ^ byte) * 0x100000001b3ull;
		}

		return hash;
	}
}
