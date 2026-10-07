#pragma once

#include <ember/core/common.h>
#include <ember/core/filesystem.h>
#include <ember/core/hash.h>
#include <ember/core/result.h>

namespace ember
{
	using AssetId = u64;

	/**
	 * An asset's name as a number: FNV-1a over its path below the root or a mount, reading either
	 * separator as '/', so a path spelt the Windows way names the same asset.
	 */
	[[nodiscard]] constexpr AssetId asset_id(StringView path) noexcept
	{
		u64 hash = HASH_SEED;

		for (const char c : path)
			hash = (hash ^ static_cast<u8>(c == '\\' ? '/' : c)) * 0x100000001b3ull;

		return hash;
	}

	/**
	 * Where names come from: the root directory, a mounted directory, a pack, anything that can hand
	 * out bytes for a name. The manager routes a name to its source by prefix and a loader asks the
	 * manager for bytes by name, never learning which source answered. Names here are below the
	 * source: the manager has taken the mount's prefix off.
	 *
	 * read() and deliver() run on the IO thread; the rest may be asked from any thread.
	 */
	class AssetSource
	{
	public:
		virtual ~AssetSource() noexcept = default;

		AssetSource(const AssetSource&)			   = delete;
		AssetSource& operator=(const AssetSource&) = delete;

		/** Whether the source serves this name now. */
		[[nodiscard]] virtual bool contains(StringView name) const noexcept = 0;

		/** The bytes behind a name: a file read whole, or a slice of a pack. NotFound when it has none. */
		[[nodiscard]] virtual Result<fs::FileData, fs::FileError> read(StringView name,
																	   std::pmr::memory_resource& memory) noexcept = 0;

		/**
		 * New bytes for a name, from another machine. A directory writes the file, atomically, making
		 * the directories on the way. A pack cannot be rewritten, so it keeps the bytes in an overlay
		 * that read() and contains() consult first.
		 */
		[[nodiscard]] virtual Result<void, fs::FileError> deliver(StringView name, Span<const u8> bytes) noexcept = 0;

		/**
		 * The absolute file behind a name, for a tool that reads and writes files itself: a cooker.
		 * False for a source that has none, such as a pack; a loader needs no path, it reads by name.
		 */
		[[nodiscard]] virtual bool file_of(StringView name, String& out) const noexcept
		{
			(void)name;
			(void)out;
			return false;
		}

		/** The name of an absolute file this source serves, for a dependency given as a path. */
		[[nodiscard]] virtual bool name_of(StringView file, String& out) const noexcept
		{
			(void)file;
			(void)out;
			return false;
		}

		/** The directory an OS watch reports saves under; empty when nothing changes behind the source's back. */
		[[nodiscard]] virtual StringView watch_directory() const noexcept { return {}; }

		/**
		 * Every name the source serves below `below` ("scripts", or empty for all of them), appended to `out`
		 * in no particular order, as the source names them: a directory walks its tree, a pack its index
		 * with what was delivered over it. Owner thread, at a start: a library that loads a whole directory.
		 * Unsupported for a source that cannot list itself.
		 */
		[[nodiscard]] virtual Result<void, fs::FileError> enumerate(StringView below, Vector<String>& out) noexcept
		{
			(void)below;
			(void)out;
			return fail(fs::FileError{.code = fs::FileErrorCode::Unsupported, .op = fs::FileOp::Enumerate});
		}

	protected:
		AssetSource() noexcept = default;
	};

	/** A directory on disk: the root, or a mount made from a path. A name is a path below it. */
	class DirectorySource final : public AssetSource
	{
	public:
		/** `directory` absolute, normalised and ending in a separator: what AssetManager resolves a path to. */
		explicit DirectorySource(StringView directory) noexcept;

		[[nodiscard]] StringView directory() const noexcept { return m_directory; }

		[[nodiscard]] bool contains(StringView name) const noexcept override;
		[[nodiscard]] Result<fs::FileData, fs::FileError> read(StringView name,
															   std::pmr::memory_resource& memory) noexcept override;
		[[nodiscard]] Result<void, fs::FileError> deliver(StringView name, Span<const u8> bytes) noexcept override;
		[[nodiscard]] bool file_of(StringView name, String& out) const noexcept override;
		[[nodiscard]] bool name_of(StringView file, String& out) const noexcept override;
		[[nodiscard]] StringView watch_directory() const noexcept override { return m_directory; }
		[[nodiscard]] Result<void, fs::FileError> enumerate(StringView below, Vector<String>& out) noexcept override;

	private:
		String m_directory;
	};
}
