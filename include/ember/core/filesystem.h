#pragma once

#include "ember/containers/span.h"
#include <ember/core/bitmask.h>
#include <ember/core/common.h>
#include <ember/core/result.h>

namespace ember::fs
{
	enum class FileErrorCode : u8
	{
		NotFound,
		AlreadyExists,
		AccessDenied,
		InvalidArgument,
		InvalidPath,
		NameTooLong,
		NotDirectory,
		IsDirectory,
		DirectoryNotEmpty,
		NoSpace,
		ReadOnly,
		Busy,
		TooLarge,
		UnexpectedEndOfFile,
		CrossDevice,
		Unsupported,
		Io,
		Unknown,
	};

	enum class FileOp : u8
	{
		Open,
		Close,
		Read,
		Write,
		Resize,
		Flush,
		Stat,
		Enumerate,
		CreateDirectory,
		Remove,
		Rename,
		Copy,
		Replace,
		ResolvePath,
		Count
	};

	/**
	 * Code is suitable for portable control flow. Operation and native_code retain
	 * enough leaf information to diagnose the original operating system failure.
	 *
	 * Paths are intentionally not stored because most callers already own the path,
	 * and retaining one here would add allocation and lifetime concerns to every error.
	 */
	struct FileError
	{
		FileErrorCode code = FileErrorCode::Unknown;
		FileOp op		   = FileOp::Open;
		u32 native_code	   = 0;

		[[nodiscard]] friend constexpr bool operator==(const FileError&, const FileError&) noexcept = default;
	};

	enum class Access : u8
	{
		Read,
		Write,
		ReadWrite,
	};

	enum class Create : u8
	{
		OpenExisting,
		CreateNew,
		OpenOrCreat,
		CreateAlways,
		TruncateExisting,
	};

	enum class Usage : u8
	{
		Normal,
		Sequential,
		Random,
	};

	enum class FileShare : u8
	{
		None   = 0,
		Read   = 1 << 0,
		Write  = 1 << 1,
		Delete = 1 << 2,
	};

	EMBER_ENUM_BITWISE_OPS(FileShare, u8);

	enum class FollowSymlinks : u8
	{
		No,
		Yes,
	};

	struct OpenOptions
	{
		Access access = Access::Read;
		Create create = Create::OpenExisting;
		Usage usage	  = Usage::Normal;

		/**
		 * Broad sharing lets development tools and hot reload replace content while
		 * the engine is reading it. Writers that require exclusivity opt out.
		 */
		FileShare share = FileShare::Read | FileShare::Write | FileShare::Delete;

		FollowSymlinks follow_symlinks = FollowSymlinks::Yes;
	};

	/**
	 * A move-only operation system file handle.
	 *
	 * read_at() and write_at() are positional. They do not mutate a shared cursor,
	 * so independent ranges may be accessed concurrently when the platform supports it.
	 * Callers must still coordinate overlapping writes and lifetime with close().
	 */
	class File final
	{
	public:
		File() noexcept = default;
		~File() noexcept;

		File(File&& other) noexcept;
		File& operator=(File&& other) noexcept;

		File(const File&)			 = delete;
		File& operator=(const File&) = delete;

		[[nodiscard]] bool is_open() const noexcept;
		[[nodiscard]] explicit operator bool() const noexcept { return is_open(); }

		/**
		 * Explicit close reports delayed operating system failures. The destructor
		 * closes as a safety net but cannot report an error.
		 */
		[[nodiscard]] Result<void, FileError> close() noexcept;

		[[nodiscard]] Result<u64, FileError> size() const noexcept;
		[[nodiscard]] Result<void, FileError> resize(u64 size) noexcept;

		/** A successful short read is possible at end of file. */
		[[nodiscard]] Result<size_t, FileError> read_at(u64 offset, Span<u8> destination) const noexcept;

		/** Reads the entire destination or returns UnexpectedEndOfFile. */
		[[nodiscard]] Result<void, FileError> read_exact_at(u64 offset, Span<u8> destination) const noexcept;

		/** A successful short write is possible on some operating systems. */
		[[nodiscard]] Result<size_t, FileError> write_at(u64 offset, Span<const u8> source) noexcept;

		/** Writes the entire source or returns an error. */
		[[nodiscard]] Result<void, FileError> write_exact_at(u64 offset, Span<const u8> source) noexcept;

		[[nodiscard]] Result<void, FileError> flush() noexcept;

	private:
		friend Result<File, FileError> open(StringView, const OpenOptions&) noexcept;

		static constexpr uintptr_t INVALID_NATIVE_HANDLE = ~uintptr_t{0};

		explicit File(uintptr_t native) noexcept : m_native(native) {}

		uintptr_t m_native = INVALID_NATIVE_HANDLE;
	};

	/**
	 * Opens one physical file.
	 *
	 * Paths are UTF-8. '/' is the only accepted directory separator. Backslashes are
	 * rejected so the same input cannot mean a filename on one platform and a directory
	 * traversal on another.
	 */
	[[nodiscard]] Result<File, FileError> open(StringView path, const OpenOptions& options = {}) noexcept;

	inline constexpr size_t FILE_DATA_ALIGNMENT = 16;
	inline constexpr u64 DEFAULT_MAX_FILE_BYTES = 256_mb;

	struct ReadFileOptions
	{
		/**
		 * Whole-file reads are intended for bounded metadata and asset blobs.
		 * Larget streaming resources should remain open and use range reads.
		 */
		u64 max_bytes	 = DEFAULT_MAX_FILE_BYTES;
		size_t alignment = FILE_DATA_ALIGNMENT;
	};

	/**
	 * An owned whole-file allocation.
	 *
	 * Ownership includes the exact memory resource and alignment needed to release
	 * the allocation. Moving the object transfers that complete ownership contract.
	 */
	class FileData final
	{
	public:
		FileData() noexcept = default;
		~FileData() noexcept;

		FileData(FileData&& other) noexcept;
		FileData& operator=(FileData&& other) noexcept;

		FileData(const FileData&)			 = delete;
		FileData& operator=(const FileData&) = delete;

		[[nodiscard]] Span<u8> bytes() noexcept;
		[[nodiscard]] Span<const u8> bytes() const noexcept;

		/**
		 * The returned text is not null terminated. It remains  valid until this
		 * object is reset, moved from or destroyed.
		 */
		[[nodiscard]] StringView text() const noexcept;

		[[nodiscard]] u8* data() noexcept { return m_data; }

		[[nodiscard]] const u8* data() const noexcept { return m_data; }

		[[nodiscard]] size_t size() const noexcept { return m_size; }

		[[nodiscard]] bool empty() const noexcept { return m_size == 0; }

		void reset() noexcept;

	private:
		friend Result<FileData, FileError> read_file(StringView, std::pmr::memory_resource&,
													 const ReadFileOptions&) noexcept;

		FileData(u8* data, size_t size, size_t alignment, std::pmr::memory_resource& memory) noexcept
			: m_data(data), m_size(size), m_alignment(alignment), m_memory(&memory)
		{
		}

		u8* m_data							= nullptr;
		size_t m_size						= 0;
		size_t m_alignment					= 0;
		std::pmr::memory_resource* m_memory = nullptr;
	};

	[[nodiscard]] Result<FileData, FileError> read_file(StringView path, std::pmr::memory_resource& memory,
														const ReadFileOptions& options = {}) noexcept;

	enum class WriteDurability : u8
	{
		/** The operation reaches the operating system but does not force stable storage. */
		None,

		/** File contents are flushed before success is reported. */
		FileData,

		/**
		 * File contents and the containing directory entry are flushed.
		 *
		 * A platform or filesystem that cannot provide this guarantee returns
		 * Unsupported instead of silently weakening the requested contract.
		 */
		FileDataAndDirectory,
	};

	/**
	 * Replaces the destination contents directly.
	 *
	 * A failure can leave a partial destination. Use write_file_atomic() for saves,
	 * manifests and other files that must retain the previous complete version.
	 */
	[[nodiscard]] Result<void, FileError> write_file(StringView path, Span<const u8> bytes,
													 WriteDurability durability = WriteDurability::None) noexcept;

	/**
	 * Writes to an exclusively created sibling and atomically replaces the target.
	 *
	 * Atomically prevents observers from seeing partial contents. Durability controls
	 * what survives a sudden power loss and is a separate guarantee.
	 */
	[[nodiscard]] Result<void, FileError>
	write_file_atomic(StringView path, Span<const u8> bytes,
					  WriteDurability durability = WriteDurability::FileDataAndDirectory) noexcept;

	enum class FileType : u8
	{
		Regular,
		Directory,
		Symlink,
		Other
	};

	struct FileInfo
	{
		FileType type		 = FileType::Other;
		u64 size			 = 0;
		i64 modified_time_ns = 0;
	};

	[[nodiscard]] Result<FileInfo, FileError> status(StringView path,
													 FollowSymlinks follow_symlinks = FollowSymlinks::Yes) noexcept;

	/** False means the path is absent. Permission and device failures remain errors. */
	[[nodiscard]] Result<bool, FileError> exists(StringView path,
												 FollowSymlinks follow_symlinks = FollowSymlinks::Yes) noexcept;

	enum class Visit : u8
	{
		Continue,
		Stop,
	};

	struct DirectoryEntry
	{
		/**
		 * Name contains one child name, never a full path. It is borrowed only for the
		 * callback and must be copied if the caller needs to retain it.
		 */
		StringView name;
		FileType type = FileType::Other;
	};

	using EnumerateFn = Visit (*)(const DirectoryEntry&, void*) noexcept;

	/**
	 * Enumerates immediate children without allocating one string per entry.
	 *
	 * Entry types do not follow symlinks. Returning Stop ends enumeration normally.
	 */
	[[nodiscard]] Result<void, FileError> enumerate(StringView directory, EnumerateFn visitor, void* data) noexcept;

	template <typename Visitor>
	concept DirectoryVisitor = std::is_object_v<std::remove_reference_t<Visitor>> &&
							   requires(std::remove_reference_t<Visitor>& visitor, const DirectoryEntry& entry) {
								   { visitor(entry) } noexcept -> std::same_as<Visit>;
							   };

	template <DirectoryVisitor Visitor>
	[[nodiscard]] Result<void, FileError> enumerate(StringView directory, Visitor&& visitor) noexcept
	{
		using VisitorType = std::remove_reference_t<Visitor>;

		return enumerate(
			directory, [](const DirectoryEntry& entry, void* data) noexcept -> Visit
			{ return (*static_cast<VisitorType*>(data))(entry); },
			const_cast<void*>(static_cast<const void*>(std::addressof(visitor))));
	}

	[[nodiscard]] Result<void, FileError> create_directory(StringView path) noexcept;

	[[nodiscard]] Result<void, FileError> create_directories(StringView path) noexcept;

	[[nodiscard]] Result<void, FileError> remove_file(StringView path) noexcept;

	[[nodiscard]] Result<void, FileError> remove_empty_directory(StringView path) noexcept;

	/**
	 * Removes a hierarchy without following symlinks or reparse points.
	 *
	 * Empty paths, current-directory aliases, parent escapes and filesystem roots
	 * are rejected. The native implementation uses directory handles so a concurrent
	 * rename cannot turn a checked child into traversal outside the requested tree.
	 */
	[[nodiscard]] Result<void, FileError> remove_tree(StringView path) noexcept;

	[[nodiscard]] Result<void, FileError> copy_file(StringView from, StringView to, bool replace = false) noexcept;

	[[nodiscard]] Result<void, FileError> rename(StringView from, StringView to, bool replace = false) noexcept;

	[[nodiscard]] bool is_absolute(StringView path) noexcept;

	[[nodiscard]] StringView file_name(StringView path) noexcept;

	[[nodiscard]] StringView extension(StringView path) noexcept;

	[[nodiscard]] StringView stem(StringView path) noexcept;

	[[nodiscard]] StringView parent(StringView path) noexcept;

	/**
	 * Performs lexical normalization without touching the filesystem.
	 *
	 * Repeated separators and "." are removed. ".." removes one preceding component.
	 * An absolute path that attempts to escape its root is rejected. Relative leading
	 * ".." components are retained.
	 *
	 * output keeps its existing allocator and is not modified when validation fails.
	 */
	[[nodiscard]] Result<void, FileError> normalize_lexical(StringView path, String& output) noexcept;

	/**
	 * Joins a relative right side to left and lexically normalizes the result.
	 *
	 * An absolute right side is rejected instead of silently discarding left.
	 */
	[[nodiscard]] Result<void, FileError> join(String& output, StringView left, StringView right) noexcept;

	/**
	 * Produces a lexically normalized absolute path. It does not resolve symlinks.
	 */
	[[nodiscard]] Result<void, FileError> absolute(StringView path, String& output) noexcept;

	[[nodiscard]] Result<void, FileError> working_directory(String& output) noexcept;

	[[nodiscard]] Result<void, FileError> executable_directory(String& output) noexcept;

	/**
	 * app must be one portable directory component. These functions only construct
	 * paths; callers decide whether and when to create them.
	 */
	[[nodiscard]] Result<void, FileError> user_data_directory(String& output, StringView app) noexcept;

	[[nodiscard]] Result<void, FileError> user_cache_directory(String& output, StringView app) noexcept;

	[[nodiscard]] Result<void, FileError> temporary_directory(String& output) noexcept;
}

namespace ember
{
	EMBER_ENUM_NAMES(fs::FileErrorCode, "NotFound", "AlreadyExists", "AccessDenied", "InvalidArgument", "InvalidPath",
					 "NameTooLong", "NotDirectory", "IsDirectory", "DirectoryNotEmpty", "NoSpace", "ReadOnly", "Busy",
					 "TooLarge", "UnexpectedEndOfFile", "CrossDevice", "Unsupported", "Io", "Unknown");

	EMBER_ENUM_NAMES(fs::FileOp, "Open", "Close", "Read", "Write", "Resize", "Flush", "Stat", "Enumerate",
					 "CreateDirectory", "Remove", "Rename", "Copy", "Replace", "ResolvePath");
}
