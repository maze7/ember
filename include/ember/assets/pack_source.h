#pragma once

#include <ember/assets/asset_source.h>
#include <ember/core/common.h>
#include <ember/memory/memory.h>
#include <ember/sync/spin_mutex.h>

namespace ember
{
	/** One file for PackSource::write(): its name below the pack's prefix, and its bytes. */
	struct PackEntry
	{
		StringView name;
		Span<const u8> bytes;
	};

	/**
	 * A pack: many files in one, read in place with positional reads, so a mount can be a single
	 * binary instead of a tree of files. The index is read once at open(); a read opens nothing.
	 *
	 *   header   "EMPK", version, count, index bytes
	 *   index    per entry: offset, size, hash_bytes() of the bytes, the name
	 *   blobs    each entry's bytes, 16 byte aligned
	 *
	 * A pack is never rewritten under the game. A file delivered to it from another machine goes
	 * into an overlay in memory, which shadows the pack's own copy until the process ends; the
	 * next build of the pack carries it properly.
	 */
	class PackSource final : public AssetSource
	{
	public:
		explicit PackSource(MemoryTag tag = MemoryTag::Assets) noexcept;
		~PackSource() noexcept override;

		/** Opens a pack and reads its index. Not a pack, or a torn one, is InvalidArgument. */
		[[nodiscard]] Result<void, fs::FileError> open(StringView path) noexcept;

		/** Writes a pack whole, atomically: a tool's job, and the tests'. Entries keep the order given. */
		[[nodiscard]] static Result<void, fs::FileError> write(StringView path, Span<const PackEntry> entries) noexcept;

		/** Entries in the index, the overlay aside. */
		[[nodiscard]] u32 count() const noexcept { return static_cast<u32>(m_entries.size()); }

		/** Names delivered since open(), which shadow the pack's own. */
		[[nodiscard]] u32 overlaid() const noexcept;

		[[nodiscard]] bool contains(StringView name) const noexcept override;
		[[nodiscard]] Result<fs::FileData, fs::FileError> read(StringView name,
															   std::pmr::memory_resource& memory) noexcept override;
		[[nodiscard]] Result<void, fs::FileError> deliver(StringView name, Span<const u8> bytes) noexcept override;

	private:
		struct Entry
		{
			String name;
			u64 offset = 0;
			u64 size   = 0;
			u64 hash   = 0;
		};

		MemoryTag m_tag;
		fs::File m_file;
		Vector<Entry> m_entries;
		HashMap<AssetId, u32> m_index; // into m_entries

		/// The overlay: read on the IO thread, written there too, asked about from anywhere.
		mutable SpinMutex m_lock;
		HashMap<AssetId, Vector<u8>> m_overlay;
	};
}
