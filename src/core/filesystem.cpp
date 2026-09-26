#include <ember/core/filesystem.h>

#include "filesystem_native.h"

#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <limits>
#include <memory_resource>
#include <system_error>
#include <utility>

namespace ember::fs
{
	namespace
	{
		static_assert(sizeof(size_t) <= sizeof(u64));

		constexpr size_t SCRATCH_BYTES	   = 2048;
		constexpr u32 TEMP_CREATE_ATTEMPTS = 64;

		struct ScratchMemory
		{
			alignas(std::max_align_t) std::array<std::byte, SCRATCH_BYTES> storage{};

			std::pmr::monotonic_buffer_resource resource{storage.data(), storage.size(),
														 std::pmr::new_delete_resource()};
		};

		struct ComponentMark
		{
			size_t erase_to = 0;
			bool parent		= false;
		};

		constinit std::atomic<u64> s_temp_sequence{0};

		[[nodiscard]] constexpr FileError make_error(FileErrorCode code, FileOp operation, u32 native_code = 0) noexcept
		{
			return {
				.code		 = code,
				.op			 = operation,
				.native_code = native_code,
			};
		}

		[[nodiscard]] constexpr bool ascii_alpha(char value) noexcept
		{
			return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
		}

		[[nodiscard]] Result<void, FileError> validate_path(StringView path, FileOp operation) noexcept
		{
			if (path.empty())
			{
				return fail(make_error(FileErrorCode::InvalidPath, operation));
			}

			for (const char value : path)
			{
				/**
				 * Embedded nulls would let validation inspect one path while the
				 * native API opens a shorter one. Backslashes have platform-dependent
				 * meaning, so the public contract rejects them instead.
				 */
				if (value == '\0' || value == '\\')
				{
					return fail(make_error(FileErrorCode::InvalidPath, operation));
				}
			}

			return {};
		}

		[[nodiscard]] constexpr bool valid_io_range(u64 offset, size_t size) noexcept
		{
			return static_cast<u64>(size) <= std::numeric_limits<u64>::max() - offset;
		}

		/**
		 * Returns the number of bytes occupied by a lexical root.
		 *
		 * UNC roots do not include a trailing slash. POSIX and drive roots do.
		 * Malformed UNC-looking paths fall back to a POSIX root and are rejected
		 * by normalize_lexical() when normalization is requested.
		 */
		[[nodiscard]] size_t root_length(StringView path) noexcept
		{
			if (path.empty())
				return 0;

			if (path.size() >= 3 && ascii_alpha(path[0]) && path[1] == ':' && path[2] == '/')
			{
				return 3;
			}

			if (path[0] != '/')
				return 0;

			if (path.size() < 2 || path[1] != '/')
				return 1;

			if (path.size() >= 3 && path[2] == '/')
				return 1;

			const size_t server_end = path.find('/', 2);

			if (server_end == StringView::npos || server_end == 2)
			{
				return 1;
			}

			size_t share_start = server_end + 1;

			while (share_start < path.size() && path[share_start] == '/')
			{
				++share_start;
			}

			if (share_start == path.size())
				return 1;

			const size_t share_end = path.find('/', share_start);

			return share_end == StringView::npos ? path.size() : share_end;
		}

		[[nodiscard]] bool is_root_path(StringView normalized) noexcept
		{
			const size_t root = root_length(normalized);

			return root != 0 && root == normalized.size();
		}

		void append_decimal(String& output, u64 value) noexcept
		{
			char buffer[32];

			const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);

			EMBER_ASSERT(error == std::errc{});

			if (error == std::errc{})
				output.append(buffer, end);
		}

		void make_temp_path(String& output, StringView target, u64 sequence) noexcept
		{
			const StringView directory = parent(target);

			output.clear();

			if (!directory.empty())
			{
				output.append(directory.data(), directory.size());

				if (output.back() != '/')
					output.push_back('/');
			}

			/**
			 * The target name is not copied into the temporary name. This keeps
			 * the temporary component below common NAME_MAX limits even when the
			 * target itself is near that limit.
			 */
			output.append(".ember-tmp-");
			append_decimal(output, detail::native_process_id());

			output.push_back('-');
			append_decimal(output, sequence);
		}

		[[nodiscard]] Result<void, FileError> finish_write(File& file, StringView path, WriteDurability durability,
														   bool sync_parent) noexcept
		{
			if (durability != WriteDurability::None)
			{
				auto flushed = file.flush();

				if (!flushed)
				{
					const FileError error = flushed.error();

					(void)file.close();
					return fail(error);
				}
			}

			auto closed = file.close();

			if (!closed)
				return fail(closed.error());

			if (sync_parent && durability == WriteDurability::FileDataAndDirectory)
			{
				auto synced = detail::native_sync_parent(path);

				if (!synced)
					return fail(synced.error());
			}

			return {};
		}

		using DirectoryQuery = Result<void, FileError> (*)(String&) noexcept;

		[[nodiscard]] Result<void, FileError> query_directory(String& output, DirectoryQuery query) noexcept
		{
			String temporary{output.get_allocator().resource()};

			auto result = query(temporary);

			if (!result)
				return fail(result.error());

			output = std::move(temporary);
			return {};
		}

		[[nodiscard]] Result<void, FileError> validate_app_component(StringView app) noexcept
		{
			if (app.empty() || app == "." || app == "..")
			{
				return fail(make_error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
			}

			for (const char raw : app)
			{
				const auto value = static_cast<unsigned char>(raw);

				/**
				 * These are invalid Windows filename characters. Rejecting them
				 * everywhere keeps one application identifier portable.
				 */
				if (value < 32 || raw == '/' || raw == '\\' || raw == '<' || raw == '>' || raw == ':' || raw == '"' ||
					raw == '|' || raw == '?' || raw == '*')
				{
					return fail(make_error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
				}
			}

			/**
			 * Windows strips trailing spaces and dots from normal path components.
			 * Rejecting them prevents two spellings from resolving to one directory.
			 */
			if (app.back() == ' ' || app.back() == '.')
			{
				return fail(make_error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
			}

			return {};
		}

		[[nodiscard]] Result<void, FileError> app_directory(String& output, StringView app,
															DirectoryQuery query) noexcept
		{
			auto valid = validate_app_component(app);

			if (!valid)
				return fail(valid.error());

			String base{output.get_allocator().resource()};

			auto found = query(base);

			if (!found)
				return fail(found.error());

			return join(output, base, app);
		}
	}

	File::~File() noexcept { (void)close(); }

	File::File(File&& other) noexcept : m_native(std::exchange(other.m_native, INVALID_NATIVE_HANDLE)) {}

	File& File::operator=(File&& other) noexcept
	{
		if (this == &other)
			return *this;

		(void)close();

		m_native = std::exchange(other.m_native, INVALID_NATIVE_HANDLE);

		return *this;
	}

	bool File::is_open() const noexcept { return m_native != INVALID_NATIVE_HANDLE; }

	Result<void, FileError> File::close() noexcept
	{
		if (!is_open())
			return {};

		/**
		 * Invalidate first so a failed native close cannot produce a second close
		 * attempt from the destructor.
		 */
		const uintptr_t native = std::exchange(m_native, INVALID_NATIVE_HANDLE);

		return detail::native_close(native);
	}

	Result<u64, FileError> File::size() const noexcept
	{
		if (!is_open())
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Stat));
		}

		return detail::native_size(m_native);
	}

	Result<void, FileError> File::resize(u64 new_size) noexcept
	{
		if (!is_open())
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Resize));
		}

		return detail::native_resize(m_native, new_size);
	}

	Result<size_t, FileError> File::read_at(u64 offset, Span<u8> destination) const noexcept
	{
		if (!is_open())
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Read));
		}

		if (!valid_io_range(offset, destination.size()))
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Read));
		}

		if (destination.empty())
			return size_t{0};

		return detail::native_read_at(m_native, offset, destination);
	}

	Result<void, FileError> File::read_exact_at(u64 offset, Span<u8> destination) const noexcept
	{
		if (!valid_io_range(offset, destination.size()))
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Read));
		}

		size_t completed = 0;

		while (completed < destination.size())
		{
			auto read = read_at(offset + static_cast<u64>(completed), destination.subspan(completed));

			if (!read)
				return fail(read.error());

			if (read.value() == 0)
			{
				return fail(make_error(FileErrorCode::UnexpectedEndOfFile, FileOp::Read));
			}

			EMBER_ASSERT(read.value() <= destination.size() - completed);

			completed += read.value();
		}

		return {};
	}

	Result<size_t, FileError> File::write_at(u64 offset, Span<const u8> source) noexcept
	{
		if (!is_open())
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Write));
		}

		if (!valid_io_range(offset, source.size()))
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Write));
		}

		if (source.empty())
			return size_t{0};

		return detail::native_write_at(m_native, offset, source);
	}

	Result<void, FileError> File::write_exact_at(u64 offset, Span<const u8> source) noexcept
	{
		if (!valid_io_range(offset, source.size()))
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Write));
		}

		size_t completed = 0;

		while (completed < source.size())
		{
			auto written = write_at(offset + static_cast<u64>(completed), source.subspan(completed));

			if (!written)
				return fail(written.error());

			/**
			 * A zero-byte success would otherwise spin forever. Treat it as an
			 * I/O failure because the requested non-empty range made no progress.
			 */
			if (written.value() == 0)
			{
				return fail(make_error(FileErrorCode::Io, FileOp::Write));
			}

			EMBER_ASSERT(written.value() <= source.size() - completed);

			completed += written.value();
		}

		return {};
	}

	Result<void, FileError> File::flush() noexcept
	{
		if (!is_open())
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Flush));
		}

		return detail::native_flush(m_native);
	}

	Result<File, FileError> open(StringView path, const OpenOptions& options) noexcept
	{
		auto valid = validate_path(path, FileOp::Open);

		if (!valid)
			return fail(valid.error());

		auto native = detail::native_open(path, options);

		if (!native)
			return fail(native.error());

		return File{native.value()};
	}

	FileData::~FileData() noexcept { reset(); }

	FileData::FileData(FileData&& other) noexcept
		: m_data(std::exchange(other.m_data, nullptr)), m_size(std::exchange(other.m_size, 0)),
		  m_alignment(std::exchange(other.m_alignment, 0)), m_memory(std::exchange(other.m_memory, nullptr))
	{
	}

	FileData& FileData::operator=(FileData&& other) noexcept
	{
		if (this == &other)
			return *this;

		reset();

		m_data = std::exchange(other.m_data, nullptr);

		m_size = std::exchange(other.m_size, 0);

		m_alignment = std::exchange(other.m_alignment, 0);

		m_memory = std::exchange(other.m_memory, nullptr);

		return *this;
	}

	Span<u8> FileData::bytes() noexcept { return {m_data, m_size}; }

	Span<const u8> FileData::bytes() const noexcept { return {m_data, m_size}; }

	StringView FileData::text() const noexcept
	{
		if (m_size == 0)
			return {};

		return {reinterpret_cast<const char*>(m_data), m_size};
	}

	void FileData::reset() noexcept
	{
		if (m_data != nullptr)
		{
			EMBER_ASSERT(m_memory != nullptr);

			m_memory->deallocate(m_data, m_size, m_alignment);
		}

		m_data		= nullptr;
		m_size		= 0;
		m_alignment = 0;
		m_memory	= nullptr;
	}

	Result<FileData, FileError> read_file(StringView path, std::pmr::memory_resource& memory,
										  const ReadFileOptions& options) noexcept
	{
		if (options.alignment == 0 || !std::has_single_bit(options.alignment))
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Read));
		}

		const OpenOptions open_options{
			.access			 = Access::Read,
			.create			 = Create::OpenExisting,
			.usage			 = Usage::Sequential,
			.share			 = FileShare::Read | FileShare::Write | FileShare::Delete,
			.follow_symlinks = FollowSymlinks::Yes,
		};

		auto opened = open(path, open_options);

		if (!opened)
			return fail(opened.error());

		File file = std::move(opened.value());

		auto measured = file.size();

		if (!measured)
			return fail(measured.error());

		const u64 byte_count = measured.value();

		if (byte_count > options.max_bytes || byte_count > static_cast<u64>(std::numeric_limits<size_t>::max()))
		{
			return fail(make_error(FileErrorCode::TooLarge, FileOp::Read));
		}

		if (byte_count == 0)
		{
			auto closed = file.close();

			if (!closed)
				return fail(closed.error());

			return FileData{};
		}

		const size_t allocation_size = static_cast<size_t>(byte_count);

		u8* allocation = static_cast<u8*>(memory.allocate(allocation_size, options.alignment));

		auto read = file.read_exact_at(0, {allocation, allocation_size});

		if (!read)
		{
			const FileError error = read.error();

			memory.deallocate(allocation, allocation_size, options.alignment);

			(void)file.close();
			return fail(error);
		}

		auto closed = file.close();

		if (!closed)
		{
			const FileError error = closed.error();

			memory.deallocate(allocation, allocation_size, options.alignment);

			return fail(error);
		}

		return FileData{allocation, allocation_size, options.alignment, memory};
	}

	Result<void, FileError> write_file(StringView path, Span<const u8> bytes, WriteDurability durability) noexcept
	{
		const OpenOptions options{
			.access			 = Access::Write,
			.create			 = Create::CreateAlways,
			.usage			 = Usage::Sequential,
			.share			 = FileShare::Read | FileShare::Delete,
			.follow_symlinks = FollowSymlinks::Yes,
		};

		auto opened = open(path, options);

		if (!opened)
			return fail(opened.error());

		File file = std::move(opened.value());

		auto written = file.write_exact_at(0, bytes);

		if (!written)
		{
			const FileError error = written.error();

			(void)file.close();
			return fail(error);
		}

		return finish_write(file, path, durability, true);
	}

	Result<void, FileError> write_file_atomic(StringView path, Span<const u8> bytes,
											  WriteDurability durability) noexcept
	{
		auto valid = validate_path(path, FileOp::Replace);

		if (!valid)
			return fail(valid.error());

		const StringView name = file_name(path);

		if (name.empty() || name == "." || name == "..")
		{
			return fail(make_error(FileErrorCode::InvalidPath, FileOp::Replace));
		}

		ScratchMemory scratch;
		String temporary{&scratch.resource};
		File file;
		bool created = false;

		const OpenOptions options{
			.access			 = Access::Write,
			.create			 = Create::CreateNew,
			.usage			 = Usage::Sequential,
			.share			 = FileShare::None,
			.follow_symlinks = FollowSymlinks::No,
		};

		for (u32 attempt = 0; attempt < TEMP_CREATE_ATTEMPTS; ++attempt)
		{
			const u64 sequence = s_temp_sequence.fetch_add(1, std::memory_order_relaxed);

			make_temp_path(temporary, path, sequence);

			auto opened = open(temporary, options);

			if (opened)
			{
				file = std::move(opened.value());

				created = true;
				break;
			}

			if (opened.error().code != FileErrorCode::AlreadyExists)
			{
				return fail(opened.error());
			}
		}

		if (!created)
		{
			return fail(make_error(FileErrorCode::Busy, FileOp::Open));
		}

		auto written = file.write_exact_at(0, bytes);

		if (!written)
		{
			const FileError error = written.error();

			(void)file.close();
			(void)detail::native_remove_file(temporary);

			return fail(error);
		}

		auto finished = finish_write(file, temporary, durability, false);

		if (!finished)
		{
			const FileError error = finished.error();

			(void)detail::native_remove_file(temporary);

			return fail(error);
		}

		auto replaced = detail::native_replace(temporary, path, durability == WriteDurability::FileDataAndDirectory);

		if (!replaced)
		{
			const FileError error = replaced.error();

			(void)detail::native_remove_file(temporary);

			return fail(error);
		}

		return {};
	}

	Result<FileInfo, FileError> status(StringView path, FollowSymlinks follow_symlinks) noexcept
	{
		auto valid = validate_path(path, FileOp::Stat);

		if (!valid)
			return fail(valid.error());

		return detail::native_status(path, follow_symlinks);
	}

	Result<bool, FileError> exists(StringView path, FollowSymlinks follow_symlinks) noexcept
	{
		auto info = status(path, follow_symlinks);

		if (info)
			return true;

		if (info.error().code == FileErrorCode::NotFound)
		{
			return false;
		}

		return fail(info.error());
	}

	Result<void, FileError> enumerate(StringView directory, EnumerateFn visitor, void* data) noexcept
	{
		auto valid = validate_path(directory, FileOp::Enumerate);

		if (!valid)
			return fail(valid.error());

		if (visitor == nullptr)
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Enumerate));
		}

		return detail::native_enumerate(directory, visitor, data);
	}

	Result<void, FileError> create_directory(StringView path) noexcept
	{
		auto valid = validate_path(path, FileOp::CreateDirectory);

		if (!valid)
			return fail(valid.error());

		return detail::native_create_directory(path);
	}

	Result<void, FileError> create_directories(StringView path) noexcept
	{
		auto valid = validate_path(path, FileOp::CreateDirectory);

		if (!valid)
			return fail(valid.error());

		ScratchMemory scratch;
		String normalized{&scratch.resource};

		auto normalized_result = normalize_lexical(path, normalized);

		if (!normalized_result)
			return fail(normalized_result.error());

		const StringView clean{normalized};
		const size_t root = root_length(clean);

		if (clean == "." || (root != 0 && root == clean.size()))
		{
			auto info = detail::native_status(clean, FollowSymlinks::Yes);

			if (!info)
				return fail(info.error());

			if (info.value().type != FileType::Directory)
			{
				return fail(make_error(FileErrorCode::NotDirectory, FileOp::CreateDirectory));
			}

			return {};
		}

		String current{&scratch.resource};

		for (size_t cursor = root; cursor <= clean.size(); ++cursor)
		{
			const bool boundary = cursor == clean.size() || clean[cursor] == '/';

			if (!boundary)
				continue;

			if (cursor == root)
				continue;

			current.assign(clean.data(), cursor);

			auto created = detail::native_create_directory(current);

			if (!created)
				return fail(created.error());
		}

		return {};
	}

	Result<void, FileError> remove_file(StringView path) noexcept
	{
		auto valid = validate_path(path, FileOp::Remove);

		if (!valid)
			return fail(valid.error());

		return detail::native_remove_file(path);
	}

	Result<void, FileError> remove_empty_directory(StringView path) noexcept
	{
		auto valid = validate_path(path, FileOp::Remove);

		if (!valid)
			return fail(valid.error());

		return detail::native_remove_empty_directory(path);
	}

	Result<void, FileError> remove_tree(StringView path) noexcept
	{
		auto valid = validate_path(path, FileOp::Remove);

		if (!valid)
			return fail(valid.error());

		/**
		 * Validation and execution use the same normalized path. This prevents
		 * a spelling such as "cache/../other" from being checked as one target
		 * and executed as another through a concurrently changed component.
		 */
		ScratchMemory scratch;
		String normalized{&scratch.resource};

		auto result = normalize_lexical(path, normalized);

		if (!result)
			return fail(result.error());

		const bool escapes_working_directory =
			!is_absolute(normalized) && (normalized == ".." || normalized.starts_with("../"));

		if (normalized == "." || escapes_working_directory || is_root_path(normalized))
		{
			return fail(make_error(FileErrorCode::InvalidArgument, FileOp::Remove));
		}

		return detail::native_remove_tree(normalized);
	}

	Result<void, FileError> copy_file(StringView from, StringView to, bool replace) noexcept
	{
		auto valid_from = validate_path(from, FileOp::Copy);

		if (!valid_from)
			return fail(valid_from.error());

		auto valid_to = validate_path(to, FileOp::Copy);

		if (!valid_to)
			return fail(valid_to.error());

		return detail::native_copy_file(from, to, replace);
	}

	Result<void, FileError> rename(StringView from, StringView to, bool replace) noexcept
	{
		auto valid_from = validate_path(from, FileOp::Rename);

		if (!valid_from)
			return fail(valid_from.error());

		auto valid_to = validate_path(to, FileOp::Rename);

		if (!valid_to)
			return fail(valid_to.error());

		return detail::native_rename(from, to, replace);
	}

	bool is_absolute(StringView path) noexcept
	{
		if (path.empty())
			return false;

		if (path[0] == '/')
			return true;

		return path.size() >= 3 && ascii_alpha(path[0]) && path[1] == ':' && path[2] == '/';
	}

	StringView file_name(StringView path) noexcept
	{
		if (path.empty() || path.back() == '/')
		{
			return {};
		}

		const size_t root = root_length(path);

		if (root != 0 && path.size() <= root)
		{
			return {};
		}

		const size_t slash = path.rfind('/');

		return slash == StringView::npos ? path : path.substr(slash + 1);
	}

	StringView extension(StringView path) noexcept
	{
		const StringView name = file_name(path);

		const size_t dot = name.rfind('.');

		/**
		 * A leading dot alone marks a hidden filename, not an extension.
		 */
		if (dot == StringView::npos || dot == 0)
		{
			return {};
		}

		return name.substr(dot);
	}

	StringView stem(StringView path) noexcept
	{
		const StringView name = file_name(path);

		const size_t dot = name.rfind('.');

		if (dot == StringView::npos || dot == 0)
		{
			return name;
		}

		return name.substr(0, dot);
	}

	StringView parent(StringView path) noexcept
	{
		if (path.empty())
			return {};

		const size_t root = root_length(path);

		size_t end = path.size();

		while (end > root && path[end - 1] == '/')
		{
			--end;
		}

		if (end <= root)
		{
			return root != 0 ? path.substr(0, root) : StringView{};
		}

		/**
		 * A trailing separator means the path already names a directory entry.
		 */
		if (end < path.size())
			return path.substr(0, end);

		const size_t slash = path.rfind('/', end - 1);

		if (slash == StringView::npos)
			return {};

		if (slash < root)
			return path.substr(0, root);

		if (slash == 0)
			return path.substr(0, 1);

		return path.substr(0, slash);
	}

	Result<void, FileError> normalize_lexical(StringView path, String& output) noexcept
	{
		auto valid = validate_path(path, FileOp::ResolvePath);

		if (!valid)
			return fail(valid.error());

		auto* resource = output.get_allocator().resource();

		/**
		 * Copy first because path may refer into output. Building into a separate
		 * string also leaves output unchanged if later validation fails.
		 */
		String source{resource};
		source.assign(path.data(), path.size());

		String normalized{resource};
		Vector<ComponentMark> components{resource};

		const StringView input{source};

		size_t cursor	   = 0;
		bool absolute_root = false;

		if (input.size() >= 2 && ascii_alpha(input[0]) && input[1] == ':')
		{
			if (input.size() < 3 || input[2] != '/')
			{
				/**
				 * Drive-relative paths depend on per-drive process state and are
				 * too surprising for an engine API.
				 */
				return fail(make_error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
			}

			normalized.push_back(input[0]);
			normalized.append(":/");

			cursor		  = 3;
			absolute_root = true;

			while (cursor < input.size() && input[cursor] == '/')
			{
				++cursor;
			}
		}
		else if (input.size() >= 2 && input[0] == '/' && input[1] == '/' && (input.size() == 2 || input[2] != '/'))
		{
			const size_t server_end = input.find('/', 2);

			if (server_end == StringView::npos || server_end == 2)
			{
				return fail(make_error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
			}

			size_t share_start = server_end + 1;

			while (share_start < input.size() && input[share_start] == '/')
			{
				++share_start;
			}

			if (share_start == input.size())
			{
				return fail(make_error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
			}

			const size_t share_end = input.find('/', share_start);

			const size_t actual_share_end = share_end == StringView::npos ? input.size() : share_end;

			const StringView server = input.substr(2, server_end - 2);

			const StringView share = input.substr(share_start, actual_share_end - share_start);

			if (server == "." || server == ".." || share == "." || share == "..")
			{
				return fail(make_error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
			}

			normalized.append("//");
			normalized.append(server.data(), server.size());

			normalized.push_back('/');

			normalized.append(share.data(), share.size());

			cursor		  = actual_share_end;
			absolute_root = true;
		}
		else if (input[0] == '/')
		{
			normalized.push_back('/');

			cursor		  = 1;
			absolute_root = true;

			while (cursor < input.size() && input[cursor] == '/')
			{
				++cursor;
			}
		}

		auto append_component = [&](StringView component, bool is_parent) noexcept
		{
			const size_t erase_to = normalized.size();

			if (!normalized.empty() && normalized.back() != '/')
			{
				normalized.push_back('/');
			}

			normalized.append(component.data(), component.size());

			components.push_back({
				.erase_to = erase_to,
				.parent	  = is_parent,
			});
		};

		while (cursor < input.size())
		{
			while (cursor < input.size() && input[cursor] == '/')
			{
				++cursor;
			}

			const size_t begin = cursor;

			while (cursor < input.size() && input[cursor] != '/')
			{
				++cursor;
			}

			if (begin == cursor)
				continue;

			const StringView component = input.substr(begin, cursor - begin);

			if (component == ".")
				continue;

			if (component == "..")
			{
				if (!components.empty() && !components.back().parent)
				{
					normalized.resize(components.back().erase_to);

					components.pop_back();
					continue;
				}

				if (absolute_root)
				{
					return fail(make_error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
				}

				append_component(component, true);

				continue;
			}

			append_component(component, false);
		}

		if (normalized.empty())
			normalized.push_back('.');

		output = std::move(normalized);
		return {};
	}

	Result<void, FileError> join(String& output, StringView left, StringView right) noexcept
	{
		auto valid_right = validate_path(right, FileOp::ResolvePath);

		if (!valid_right)
			return fail(valid_right.error());

		if (is_absolute(right))
		{
			return fail(make_error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
		}

		if (left.empty())
		{
			return normalize_lexical(right, output);
		}

		auto valid_left = validate_path(left, FileOp::ResolvePath);

		if (!valid_left)
			return fail(valid_left.error());

		String combined{output.get_allocator().resource()};

		combined.append(left.data(), left.size());

		if (combined.back() != '/')
			combined.push_back('/');

		combined.append(right.data(), right.size());

		return normalize_lexical(combined, output);
	}

	Result<void, FileError> absolute(StringView path, String& output) noexcept
	{
		auto valid = validate_path(path, FileOp::ResolvePath);

		if (!valid)
			return fail(valid.error());

		if (is_absolute(path))
		{
			return normalize_lexical(path, output);
		}

		String working{output.get_allocator().resource()};

		auto found = working_directory(working);

		if (!found)
			return fail(found.error());

		return join(output, working, path);
	}

	Result<void, FileError> working_directory(String& output) noexcept
	{
		return query_directory(output, detail::native_working_directory);
	}

	Result<void, FileError> executable_directory(String& output) noexcept
	{
		return query_directory(output, detail::native_executable_directory);
	}

	Result<void, FileError> user_data_directory(String& output, StringView app) noexcept
	{
		return app_directory(output, app, detail::native_user_data_directory);
	}

	Result<void, FileError> user_cache_directory(String& output, StringView app) noexcept
	{
		return app_directory(output, app, detail::native_user_cache_directory);
	}

	Result<void, FileError> temporary_directory(String& output) noexcept
	{
		return query_directory(output, detail::native_temporary_directory);
	}
}
