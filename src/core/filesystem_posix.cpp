#if defined(EMBER_PLATFORM_LINUX) || defined(EMBER_PLATFORM_MACOS)

	#include "filesystem_native.h"

	#include <algorithm>
	#include <cerrno>
	#include <cstdlib>
	#include <cstring>
	#include <limits>
	#include <memory>
	#include <string>
	#include <utility>
	#include <vector>

	#include <dirent.h>
	#include <fcntl.h>
	#include <pwd.h>
	#include <sys/stat.h>
	#include <sys/types.h>
	#include <unistd.h>

	#if defined(EMBER_PLATFORM_LINUX)
		#include <linux/fs.h>
		#include <sys/syscall.h>
	#elif defined(EMBER_PLATFORM_MACOS)
		#include <mach-o/dyld.h>
		#include <stdio.h>
	#endif

namespace ember::fs::detail
{
	namespace
	{
		static_assert(sizeof(off_t) >= sizeof(i64));

		constexpr size_t COPY_BUFFER_BYTES = 256 * 1024;
		constexpr size_t MAX_PATH_BYTES	   = 1024 * 1024;

		[[nodiscard]] FileErrorCode classify_errno(int value) noexcept
		{
			switch (value)
			{
				case ENOENT:
					return FileErrorCode::NotFound;
				case EEXIST:
					return FileErrorCode::AlreadyExists;

				case EACCES:
				case EPERM:
					return FileErrorCode::AccessDenied;

				case EINVAL:
					return FileErrorCode::InvalidArgument;
				case ELOOP:
					return FileErrorCode::InvalidPath;
				case ENAMETOOLONG:
					return FileErrorCode::NameTooLong;
				case ENOTDIR:
					return FileErrorCode::NotDirectory;
				case EISDIR:
					return FileErrorCode::IsDirectory;
				case ENOTEMPTY:
					return FileErrorCode::DirectoryNotEmpty;

				case ENOSPC:
	#ifdef EDQUOT
				case EDQUOT:
	#endif
					return FileErrorCode::NoSpace;

				case EROFS:
					return FileErrorCode::ReadOnly;

				case EBUSY:
				case EMFILE:
				case ENFILE:
				case EAGAIN:
					return FileErrorCode::Busy;

				case EFBIG:
				case EOVERFLOW:
					return FileErrorCode::TooLarge;

				case EXDEV:
					return FileErrorCode::CrossDevice;

				case ENOSYS:
				case ENOTSUP:
	#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
				case EOPNOTSUPP:
	#endif
					return FileErrorCode::Unsupported;

				case EIO:
					return FileErrorCode::Io;
				default:
					return FileErrorCode::Unknown;
			}
		}

		[[nodiscard]] FileError posix_error(FileOp op, int value) noexcept
		{
			return {.code = classify_errno(value), .op = op, .native_code = static_cast<u32>(value)};
		}

		[[nodiscard]] FileError posix_error(FileOp op) noexcept { return posix_error(op, errno); }

		[[nodiscard]] FileError error(FileErrorCode code, FileOp op) noexcept
		{
			return {.code = code, .op = op, .native_code = 0};
		}

		[[nodiscard]] bool foreign_root(StringView path) noexcept
		{
			if (path.starts_with("//"))
				return true;

			if (path.size() < 3 || path[1] != ':' || path[2] != '/')
				return false;

			const char drive = path[0];
			return (drive >= 'a' && drive <= 'z') || (drive >= 'A' && drive <= 'Z');
		}

		[[nodiscard]] Result<std::string, FileError> native_path(StringView path, FileOp op) noexcept
		{
			if (foreign_root(path))
				return fail(error(FileErrorCode::InvalidPath, op));

			return std::string{path};
		}

		class Fd final
		{
		public:
			explicit Fd(int value = -1) noexcept : m_value(value) {}

			~Fd() noexcept
			{
				if (m_value >= 0)
					(void)::close(m_value);
			}

			Fd(Fd&& other) noexcept : m_value(std::exchange(other.m_value, -1)) {}

			Fd& operator=(Fd&& other) noexcept
			{
				if (this != &other)
				{
					if (m_value >= 0)
						(void)::close(m_value);

					m_value = std::exchange(other.m_value, -1);
				}
				return *this;
			}

			Fd(const Fd&)			 = delete;
			Fd& operator=(const Fd&) = delete;

			[[nodiscard]] int get() const noexcept { return m_value; }

			[[nodiscard]] int release() noexcept { return std::exchange(m_value, -1); }

			[[nodiscard]] Result<void, FileError> close(FileOp op) noexcept
			{
				if (m_value < 0)
					return {};

				const int value = release();

				/*
				 * Retrying close after EINTR can close a descriptor that another
				 * thread has since opened with the same number.
				 */
				if (::close(value) != 0)
					return fail(posix_error(op));

				return {};
			}

		private:
			int m_value;
		};

		class Dir final
		{
		public:
			explicit Dir(DIR* value = nullptr) noexcept : m_value(value) {}

			~Dir() noexcept
			{
				if (m_value != nullptr)
					(void)::closedir(m_value);
			}

			Dir(Dir&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}

			Dir& operator=(Dir&& other) noexcept
			{
				if (this != &other)
				{
					if (m_value != nullptr)
						(void)::closedir(m_value);

					m_value = std::exchange(other.m_value, nullptr);
				}
				return *this;
			}

			Dir(const Dir&)			   = delete;
			Dir& operator=(const Dir&) = delete;

			[[nodiscard]] DIR* get() const noexcept { return m_value; }

			[[nodiscard]] Result<void, FileError> close(FileOp op) noexcept
			{
				if (m_value == nullptr)
					return {};

				DIR* value = std::exchange(m_value, nullptr);
				if (::closedir(value) != 0)
					return fail(posix_error(op));

				return {};
			}

		private:
			DIR* m_value;
		};

		[[nodiscard]] FileType file_type(mode_t mode) noexcept
		{
			if (S_ISREG(mode))
				return FileType::Regular;
			if (S_ISDIR(mode))
				return FileType::Directory;
			if (S_ISLNK(mode))
				return FileType::Symlink;
			return FileType::Other;
		}

		[[nodiscard]] i64 unix_time_ns(const struct stat& info) noexcept
		{
	#if defined(EMBER_PLATFORM_MACOS)
			const i64 seconds	  = info.st_mtimespec.tv_sec;
			const i64 nanoseconds = info.st_mtimespec.tv_nsec;
	#else
			const i64 seconds	  = info.st_mtim.tv_sec;
			const i64 nanoseconds = info.st_mtim.tv_nsec;
	#endif
			constexpr i64 billion = 1'000'000'000;
			constexpr i64 maximum = std::numeric_limits<i64>::max();
			constexpr i64 minimum = std::numeric_limits<i64>::min();

			if (seconds > (maximum - nanoseconds) / billion)
				return maximum;
			if (seconds < (minimum + nanoseconds) / billion)
				return minimum;

			return seconds * billion + nanoseconds;
		}

		[[nodiscard]] bool same_file(const struct stat& a, const struct stat& b) noexcept
		{
			return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
		}

		[[nodiscard]] Result<void, FileError> sync_fd(int fd, FileOp op, bool strong) noexcept
		{
			int result;

	#if defined(EMBER_PLATFORM_MACOS)
			if (strong)
			{
				// fsync alone does not ask the drive to flush its own cache.
				do
				{
					result = ::fcntl(fd, F_FULLFSYNC);
				} while (result < 0 && errno == EINTR);
			}
			else
	#endif
			{
				(void)strong;
				do
				{
					result = ::fsync(fd);
				} while (result < 0 && errno == EINTR);
			}

			if (result == 0)
				return {};

			const int code = errno;
			if (code == EINVAL || code == ENOTSUP
	#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
				|| code == EOPNOTSUPP
	#endif
			)
			{
				return fail(posix_error(op, ENOTSUP));
			}

			return fail(posix_error(op, code));
		}

		[[nodiscard]] Result<Fd, FileError> open_parent_chain(StringView path) noexcept
		{
			if (foreign_root(path))
				return fail(error(FileErrorCode::InvalidPath, FileOp::Remove));

			const bool absolute = !path.empty() && path.front() == '/';
			Fd current{::open(absolute ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};

			if (current.get() < 0)
				return fail(posix_error(FileOp::Remove));

			size_t cursor = absolute ? 1 : 0;

			while (cursor < path.size())
			{
				while (cursor < path.size() && path[cursor] == '/')
					++cursor;

				if (cursor == path.size())
					break;

				const size_t begin = cursor;
				while (cursor < path.size() && path[cursor] != '/')
					++cursor;

				const StringView component = path.substr(begin, cursor - begin);

				if (component == ".")
					continue;

				if (component == "..")
				{
					return fail(error(FileErrorCode::InvalidPath, FileOp::Remove));
				}

				const std::string name{component};
				int next;

				do
				{
					next = ::openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
				} while (next < 0 && errno == EINTR);

				if (next < 0)
					return fail(posix_error(FileOp::Remove));

				current = Fd{next};
			}

			return std::move(current);
		}

		struct RemoveFrame
		{
			Dir directory;
			std::string name;
			int parent_fd;
			struct stat identity;

			RemoveFrame(Dir&& opened, std::string&& component, int parent, const struct stat& info) noexcept
				: directory(std::move(opened)), name(std::move(component)), parent_fd(parent), identity(info)
			{
			}

			RemoveFrame(RemoveFrame&&) noexcept			   = default;
			RemoveFrame& operator=(RemoveFrame&&) noexcept = default;
			RemoveFrame(const RemoveFrame&)				   = delete;
			RemoveFrame& operator=(const RemoveFrame&)	   = delete;
		};

		[[nodiscard]] Result<void, FileError> push_directory(std::vector<RemoveFrame>& frames, int parent_fd,
															 std::string name, const struct stat& observed,
															 dev_t tree_device) noexcept
		{
			if (observed.st_dev != tree_device)
				return fail(error(FileErrorCode::CrossDevice, FileOp::Remove));

			int descriptor;
			do
			{
				descriptor = ::openat(parent_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			} while (descriptor < 0 && errno == EINTR);

			if (descriptor < 0)
				return fail(posix_error(FileOp::Remove));

			Fd opened{descriptor};
			struct stat identity{};

			if (::fstat(opened.get(), &identity) != 0)
				return fail(posix_error(FileOp::Remove));

			if (!S_ISDIR(identity.st_mode) || !same_file(observed, identity))
			{
				return fail(error(FileErrorCode::Busy, FileOp::Remove));
			}

			DIR* raw = ::fdopendir(opened.get());
			if (raw == nullptr)
				return fail(posix_error(FileOp::Remove));

			(void)opened.release();
			frames.emplace_back(Dir{raw}, std::move(name), parent_fd, identity);
			return {};
		}

		[[nodiscard]] Result<void, FileError> home_directory(String& output) noexcept
		{
			const char* configured = std::getenv("HOME");

			if (configured != nullptr && configured[0] == '/')
			{
				output.assign(configured);
				return {};
			}

			// Services and tests may run without HOME.
			struct passwd record{};
			struct passwd* found = nullptr;
			std::vector<char> buffer(4096);

			for (;;)
			{
				const int result = ::getpwuid_r(::getuid(), &record, buffer.data(), buffer.size(), &found);

				if (result == ERANGE)
				{
					if (buffer.size() >= MAX_PATH_BYTES)
					{
						return fail(error(FileErrorCode::TooLarge, FileOp::ResolvePath));
					}

					buffer.resize(std::min(buffer.size() * 2, MAX_PATH_BYTES));
					continue;
				}

				if (result != 0)
					return fail(posix_error(FileOp::ResolvePath, result));

				if (found == nullptr || found->pw_dir == nullptr || found->pw_dir[0] != '/')
				{
					return fail(error(FileErrorCode::NotFound, FileOp::ResolvePath));
				}

				output.assign(found->pw_dir);
				return {};
			}
		}

		[[nodiscard]] Result<void, FileError> home_child(String& output, StringView suffix) noexcept
		{
			String result{output.get_allocator().resource()};
			auto found = home_directory(result);
			if (!found)
				return fail(found.error());

			while (result.size() > 1 && result.back() == '/')
				result.pop_back();

			result.append(suffix.data(), suffix.size());
			output = std::move(result);
			return {};
		}
	}

	Result<NativeHandle, FileError> native_open(StringView path, const OpenOptions& options) noexcept
	{
		auto converted = native_path(path, FileOp::Open);
		if (!converted)
			return fail(converted.error());

		if (options.access == Access::Read && options.create != Create::OpenExisting)
		{
			return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		int flags = O_CLOEXEC | O_NOCTTY | O_NONBLOCK;

		switch (options.access)
		{
			case Access::Read:
				flags |= O_RDONLY;
				break;
			case Access::Write:
				flags |= O_WRONLY;
				break;
			case Access::ReadWrite:
				flags |= O_RDWR;
				break;
			default:
				return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		switch (options.create)
		{
			case Create::OpenExisting:
				break;
			case Create::CreateNew:
				flags |= O_CREAT | O_EXCL;
				break;
			case Create::OpenOrCreat:
				flags |= O_CREAT;
				break;
			case Create::CreateAlways:
				flags |= O_CREAT | O_TRUNC;
				break;
			case Create::TruncateExisting:
				flags |= O_TRUNC;
				break;
			default:
				return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		if (options.follow_symlinks == FollowSymlinks::No)
			flags |= O_NOFOLLOW;
		else if (options.follow_symlinks != FollowSymlinks::Yes)
		{
			return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		if (options.usage != Usage::Normal && options.usage != Usage::Sequential && options.usage != Usage::Random)
		{
			return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		int descriptor;
		do
		{
			descriptor = ::open(converted->c_str(), flags, 0666);
		} while (descriptor < 0 && errno == EINTR);

		if (descriptor < 0)
			return fail(posix_error(FileOp::Open));

		Fd file{descriptor};
		struct stat info{};

		if (::fstat(file.get(), &info) != 0)
			return fail(posix_error(FileOp::Open));

		if (S_ISDIR(info.st_mode))
			return fail(error(FileErrorCode::IsDirectory, FileOp::Open));

		if (!S_ISREG(info.st_mode))
			return fail(error(FileErrorCode::Unsupported, FileOp::Open));

	#if defined(EMBER_PLATFORM_LINUX)
		// Usage is only a hint; failure does not invalidate the open file.
		if (options.usage == Usage::Sequential)
			(void)::posix_fadvise(file.get(), 0, 0, POSIX_FADV_SEQUENTIAL);
		else if (options.usage == Usage::Random)
			(void)::posix_fadvise(file.get(), 0, 0, POSIX_FADV_RANDOM);
	#endif

		return static_cast<NativeHandle>(file.release());
	}

	Result<void, FileError> native_close(NativeHandle file) noexcept
	{
		Fd descriptor{static_cast<int>(file)};
		return descriptor.close(FileOp::Close);
	}

	Result<u64, FileError> native_size(NativeHandle file) noexcept
	{
		struct stat info{};

		if (::fstat(static_cast<int>(file), &info) != 0)
			return fail(posix_error(FileOp::Stat));

		if (info.st_size < 0)
			return fail(error(FileErrorCode::Io, FileOp::Stat));

		return static_cast<u64>(info.st_size);
	}

	Result<void, FileError> native_resize(NativeHandle file, u64 size) noexcept
	{
		if (size > static_cast<u64>(std::numeric_limits<off_t>::max()))
		{
			return fail(error(FileErrorCode::TooLarge, FileOp::Resize));
		}

		int result;
		do
		{
			result = ::ftruncate(static_cast<int>(file), static_cast<off_t>(size));
		} while (result < 0 && errno == EINTR);

		if (result < 0)
			return fail(posix_error(FileOp::Resize));

		return {};
	}

	Result<size_t, FileError> native_read_at(NativeHandle file, u64 offset, Span<u8> destination) noexcept
	{
		if (offset > static_cast<u64>(std::numeric_limits<off_t>::max()))
		{
			return fail(error(FileErrorCode::TooLarge, FileOp::Read));
		}

		const size_t count = std::min(destination.size(), static_cast<size_t>(std::numeric_limits<ssize_t>::max()));

		ssize_t received;
		do
		{
			received = ::pread(static_cast<int>(file), destination.data(), count, static_cast<off_t>(offset));
		} while (received < 0 && errno == EINTR);

		if (received < 0)
			return fail(posix_error(FileOp::Read));

		return static_cast<size_t>(received);
	}

	Result<size_t, FileError> native_write_at(NativeHandle file, u64 offset, Span<const u8> source) noexcept
	{
		if (offset > static_cast<u64>(std::numeric_limits<off_t>::max()))
		{
			return fail(error(FileErrorCode::TooLarge, FileOp::Write));
		}

		const size_t count = std::min(source.size(), static_cast<size_t>(std::numeric_limits<ssize_t>::max()));

		ssize_t written;
		do
		{
			written = ::pwrite(static_cast<int>(file), source.data(), count, static_cast<off_t>(offset));
		} while (written < 0 && errno == EINTR);

		if (written < 0)
			return fail(posix_error(FileOp::Write));

		return static_cast<size_t>(written);
	}

	Result<void, FileError> native_flush(NativeHandle file) noexcept
	{
		return sync_fd(static_cast<int>(file), FileOp::Flush, true);
	}

	Result<FileInfo, FileError> native_status(StringView path, FollowSymlinks follow) noexcept
	{
		auto converted = native_path(path, FileOp::Stat);
		if (!converted)
			return fail(converted.error());

		struct stat info{};
		int result;

		if (follow == FollowSymlinks::Yes)
		{
			do
			{
				result = ::stat(converted->c_str(), &info);
			} while (result < 0 && errno == EINTR);
		}
		else if (follow == FollowSymlinks::No)
		{
			do
			{
				result = ::lstat(converted->c_str(), &info);
			} while (result < 0 && errno == EINTR);
		}
		else
		{
			return fail(error(FileErrorCode::InvalidArgument, FileOp::Stat));
		}

		if (result < 0)
			return fail(posix_error(FileOp::Stat));

		return FileInfo{.type			  = file_type(info.st_mode),
						.size			  = info.st_size < 0 ? 0 : static_cast<u64>(info.st_size),
						.modified_time_ns = unix_time_ns(info)};
	}

	Result<void, FileError> native_enumerate(StringView path, EnumerateFn visitor, void* data) noexcept
	{
		auto converted = native_path(path, FileOp::Enumerate);
		if (!converted)
			return fail(converted.error());

		Dir directory{::opendir(converted->c_str())};
		if (directory.get() == nullptr)
			return fail(posix_error(FileOp::Enumerate));

		const int fd = ::dirfd(directory.get());
		if (fd < 0)
			return fail(posix_error(FileOp::Enumerate));

		for (;;)
		{
			errno		 = 0;
			dirent* item = ::readdir(directory.get());

			if (item == nullptr)
			{
				if (errno != 0)
					return fail(posix_error(FileOp::Enumerate));
				break;
			}

			if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0)
			{
				continue;
			}

			struct stat info{};

			if (::fstatat(fd, item->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0)
			{
				if (errno == ENOENT)
					continue;

				return fail(posix_error(FileOp::Enumerate));
			}

			const DirectoryEntry entry{.name = StringView{item->d_name}, .type = file_type(info.st_mode)};

			if (visitor(entry, data) == Visit::Stop)
				break;
		}

		return directory.close(FileOp::Enumerate);
	}

	Result<void, FileError> native_create_directory(StringView path) noexcept
	{
		auto converted = native_path(path, FileOp::CreateDirectory);
		if (!converted)
			return fail(converted.error());

		if (::mkdir(converted->c_str(), 0777) == 0)
			return {};

		const int code = errno;

		if (code == EEXIST)
		{
			struct stat info{};

			if (::stat(converted->c_str(), &info) == 0 && S_ISDIR(info.st_mode))
			{
				return {};
			}
		}

		return fail(posix_error(FileOp::CreateDirectory, code));
	}

	Result<void, FileError> native_remove_file(StringView path) noexcept
	{
		auto converted = native_path(path, FileOp::Remove);
		if (!converted)
			return fail(converted.error());

		if (::unlink(converted->c_str()) != 0)
			return fail(posix_error(FileOp::Remove));

		return {};
	}

	Result<void, FileError> native_remove_empty_directory(StringView path) noexcept
	{
		auto converted = native_path(path, FileOp::Remove);
		if (!converted)
			return fail(converted.error());

		if (::rmdir(converted->c_str()) != 0)
			return fail(posix_error(FileOp::Remove));

		return {};
	}

	Result<void, FileError> native_remove_tree(StringView path) noexcept
	{
		if (foreign_root(path))
			return fail(error(FileErrorCode::InvalidPath, FileOp::Remove));

		const StringView leaf = file_name(path);
		if (leaf.empty() || leaf == "." || leaf == "..")
			return fail(error(FileErrorCode::InvalidPath, FileOp::Remove));

		auto parent_directory = open_parent_chain(parent(path));
		if (!parent_directory)
			return fail(parent_directory.error());

		const std::string root_name{leaf};
		struct stat parent_info{};
		struct stat root_info{};

		if (::fstat(parent_directory->get(), &parent_info) != 0)
			return fail(posix_error(FileOp::Remove));

		if (::fstatat(parent_directory->get(), root_name.c_str(), &root_info, AT_SYMLINK_NOFOLLOW) != 0)
		{
			return fail(posix_error(FileOp::Remove));
		}

		if (!S_ISDIR(root_info.st_mode))
		{
			if (::unlinkat(parent_directory->get(), root_name.c_str(), 0) != 0)
			{
				return fail(posix_error(FileOp::Remove));
			}
			return {};
		}

		if (root_info.st_dev != parent_info.st_dev)
			return fail(error(FileErrorCode::CrossDevice, FileOp::Remove));

		std::vector<RemoveFrame> frames;
		auto opened = push_directory(frames, parent_directory->get(), root_name, root_info, root_info.st_dev);

		if (!opened)
			return fail(opened.error());

		while (!frames.empty())
		{
			RemoveFrame& frame = frames.back();
			const int fd	   = ::dirfd(frame.directory.get());

			if (fd < 0)
				return fail(posix_error(FileOp::Remove));

			errno		 = 0;
			dirent* item = ::readdir(frame.directory.get());

			if (item != nullptr)
			{
				if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0)
				{
					continue;
				}

				std::string child_name{item->d_name};
				struct stat child{};

				if (::fstatat(fd, child_name.c_str(), &child, AT_SYMLINK_NOFOLLOW) != 0)
				{
					if (errno == ENOENT)
						continue;
					return fail(posix_error(FileOp::Remove));
				}

				if (S_ISDIR(child.st_mode))
				{
					auto descended = push_directory(frames, fd, std::move(child_name), child, root_info.st_dev);

					if (!descended)
						return fail(descended.error());
				}
				else if (::unlinkat(fd, child_name.c_str(), 0) != 0 && errno != ENOENT)
				{
					return fail(posix_error(FileOp::Remove));
				}

				continue;
			}

			if (errno != 0)
				return fail(posix_error(FileOp::Remove));

			struct stat current{};

			if (::fstatat(frame.parent_fd, frame.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0)
			{
				return fail(posix_error(FileOp::Remove));
			}

			if (!same_file(frame.identity, current))
				return fail(error(FileErrorCode::Busy, FileOp::Remove));

			if (::unlinkat(frame.parent_fd, frame.name.c_str(), AT_REMOVEDIR) != 0)
			{
				return fail(posix_error(FileOp::Remove));
			}

			auto closed = frame.directory.close(FileOp::Remove);
			frames.pop_back();

			if (!closed)
				return fail(closed.error());
		}

		return {};
	}

	Result<void, FileError> native_copy_file(StringView from, StringView to, bool replace) noexcept
	{
		auto source_path = native_path(from, FileOp::Copy);
		if (!source_path)
			return fail(source_path.error());

		auto target_path = native_path(to, FileOp::Copy);
		if (!target_path)
			return fail(target_path.error());

		int source_fd;
		do
		{
			source_fd = ::open(source_path->c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
		} while (source_fd < 0 && errno == EINTR);

		if (source_fd < 0)
			return fail(posix_error(FileOp::Copy));

		Fd source{source_fd};
		struct stat source_info{};

		if (::fstat(source.get(), &source_info) != 0)
			return fail(posix_error(FileOp::Copy));

		if (!S_ISREG(source_info.st_mode))
		{
			return fail(error(S_ISDIR(source_info.st_mode) ? FileErrorCode::IsDirectory : FileErrorCode::Unsupported,
							  FileOp::Copy));
		}

		if (source_info.st_size < 0)
			return fail(error(FileErrorCode::Io, FileOp::Copy));

		auto buffer =
			std::unique_ptr<u8, decltype(&std::free)>{static_cast<u8*>(std::malloc(COPY_BUFFER_BYTES)), &std::free};

		if (!buffer)
			return fail(posix_error(FileOp::Copy, ENOMEM));

		const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | (replace ? 0 : O_EXCL);

		int target_fd;
		do
		{
			target_fd = ::open(target_path->c_str(), flags, static_cast<mode_t>(source_info.st_mode & 0777));
		} while (target_fd < 0 && errno == EINTR);

		if (target_fd < 0)
			return fail(posix_error(FileOp::Copy));

		Fd target{target_fd};
		struct stat target_info{};

		if (::fstat(target.get(), &target_info) != 0)
			return fail(posix_error(FileOp::Copy));

		if (!S_ISREG(target_info.st_mode))
		{
			return fail(error(S_ISDIR(target_info.st_mode) ? FileErrorCode::IsDirectory : FileErrorCode::Unsupported,
							  FileOp::Copy));
		}

		// Hard links can name the same inode. Check before truncating it.
		if (same_file(source_info, target_info))
		{
			return fail(error(FileErrorCode::InvalidArgument, FileOp::Copy));
		}

		if (::ftruncate(target.get(), 0) != 0)
			return fail(posix_error(FileOp::Copy));

		/*
		 * Copy the length observed at open. Concurrent writes to the source
		 * are not a snapshot and must be coordinated by the caller.
		 */
		u64 remaining = static_cast<u64>(source_info.st_size);

		while (remaining > 0)
		{
			const size_t requested = static_cast<size_t>(std::min<u64>(remaining, COPY_BUFFER_BYTES));

			ssize_t received;
			do
			{
				received = ::read(source.get(), buffer.get(), requested);
			} while (received < 0 && errno == EINTR);

			if (received < 0)
				return fail(posix_error(FileOp::Copy));

			if (received == 0)
			{
				return fail(error(FileErrorCode::UnexpectedEndOfFile, FileOp::Copy));
			}

			size_t completed   = 0;
			const size_t count = static_cast<size_t>(received);

			while (completed < count)
			{
				ssize_t written;
				do
				{
					written = ::write(target.get(), buffer.get() + completed, count - completed);
				} while (written < 0 && errno == EINTR);

				if (written < 0)
					return fail(posix_error(FileOp::Copy));

				if (written == 0)
					return fail(error(FileErrorCode::Io, FileOp::Copy));

				completed += static_cast<size_t>(written);
			}

			remaining -= count;
		}

		auto closed_target = target.close(FileOp::Copy);
		auto closed_source = source.close(FileOp::Copy);

		if (!closed_target)
			return fail(closed_target.error());
		if (!closed_source)
			return fail(closed_source.error());

		return {};
	}

	Result<void, FileError> native_rename(StringView from, StringView to, bool replace) noexcept
	{
		auto source = native_path(from, FileOp::Rename);
		if (!source)
			return fail(source.error());

		auto target = native_path(to, FileOp::Rename);
		if (!target)
			return fail(target.error());

		int result;

		if (replace)
		{
			do
			{
				result = ::rename(source->c_str(), target->c_str());
			} while (result < 0 && errno == EINTR);
		}
	#if defined(EMBER_PLATFORM_LINUX) && defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
		else
		{
			do
			{
				result = static_cast<int>(
					::syscall(SYS_renameat2, AT_FDCWD, source->c_str(), AT_FDCWD, target->c_str(), RENAME_NOREPLACE));
			} while (result < 0 && errno == EINTR);
		}
	#elif defined(EMBER_PLATFORM_MACOS) && defined(RENAME_EXCL)
		else
		{
			do
			{
				result = ::renamex_np(source->c_str(), target->c_str(), RENAME_EXCL);
			} while (result < 0 && errno == EINTR);
		}
	#else
		else
		{
			return fail(error(FileErrorCode::Unsupported, FileOp::Rename));
		}
	#endif

		if (result < 0)
		{
			const int code = errno;

			if (!replace && (code == ENOSYS || code == EINVAL))
			{
				return fail(error(FileErrorCode::Unsupported, FileOp::Rename));
			}

			return fail(posix_error(FileOp::Rename, code));
		}

		return {};
	}

	Result<void, FileError> native_replace(StringView temporary, StringView target, bool sync_parent) noexcept
	{
		auto source = native_path(temporary, FileOp::Replace);
		if (!source)
			return fail(source.error());

		auto destination = native_path(target, FileOp::Replace);
		if (!destination)
			return fail(destination.error());

		int result;
		do
		{
			result = ::rename(source->c_str(), destination->c_str());
		} while (result < 0 && errno == EINTR);

		if (result < 0)
			return fail(posix_error(FileOp::Replace));

		/*
		 * A failed directory sync happens after replacement is visible.
		 * The returned error means durability is uncertain, not rollback.
		 */
		if (sync_parent)
			return native_sync_parent(target);

		return {};
	}

	Result<void, FileError> native_sync_parent(StringView path) noexcept
	{
		const StringView parent_path = parent(path);
		const std::string directory{parent_path.empty() ? StringView{"."} : parent_path};

		int descriptor;
		do
		{
			descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		} while (descriptor < 0 && errno == EINTR);

		if (descriptor < 0)
			return fail(posix_error(FileOp::Flush));

		Fd fd{descriptor};
		auto synced = sync_fd(fd.get(), FileOp::Flush, true);
		if (!synced)
			return fail(synced.error());

		return fd.close(FileOp::Flush);
	}

	u64 native_process_id() noexcept { return static_cast<u64>(::getpid()); }

	Result<void, FileError> native_working_directory(String& output) noexcept
	{
		String result{output.get_allocator().resource()};
		size_t capacity = 256;

		for (;;)
		{
			if (capacity > MAX_PATH_BYTES)
			{
				return fail(error(FileErrorCode::TooLarge, FileOp::ResolvePath));
			}

			result.resize(capacity);

			if (::getcwd(result.data(), result.size()) != nullptr)
			{
				result.resize(std::strlen(result.c_str()));
				output = std::move(result);
				return {};
			}

			if (errno != ERANGE)
				return fail(posix_error(FileOp::ResolvePath));

			capacity *= 2;
		}
	}

	Result<void, FileError> native_executable_directory(String& output) noexcept
	{
		String executable{output.get_allocator().resource()};

	#if defined(EMBER_PLATFORM_LINUX)
		size_t capacity = 256;

		for (;;)
		{
			if (capacity > MAX_PATH_BYTES)
			{
				return fail(error(FileErrorCode::TooLarge, FileOp::ResolvePath));
			}

			executable.resize(capacity);

			const ssize_t count = ::readlink("/proc/self/exe", executable.data(), executable.size());

			if (count < 0)
				return fail(posix_error(FileOp::ResolvePath));

			if (static_cast<size_t>(count) < executable.size())
			{
				executable.resize(static_cast<size_t>(count));
				break;
			}

			capacity *= 2;
		}
	#else
		uint32_t capacity = 256;

		for (;;)
		{
			if (capacity > MAX_PATH_BYTES)
			{
				return fail(error(FileErrorCode::TooLarge, FileOp::ResolvePath));
			}

			executable.resize(capacity);

			if (::_NSGetExecutablePath(executable.data(), &capacity) == 0)
			{
				executable.resize(std::strlen(executable.c_str()));
				break;
			}
		}

		char* resolved = ::realpath(executable.c_str(), nullptr);
		if (resolved == nullptr)
			return fail(posix_error(FileOp::ResolvePath));

		executable.assign(resolved);
		std::free(resolved);
	#endif

		const StringView directory = parent(executable);
		if (directory.empty())
		{
			return fail(error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
		}

		output.assign(directory.data(), directory.size());
		return {};
	}

	Result<void, FileError> native_user_data_directory(String& output) noexcept
	{
	#if defined(EMBER_PLATFORM_MACOS)
		return home_child(output, "/Library/Application Support");
	#else
		const char* configured = std::getenv("XDG_DATA_HOME");
		if (configured != nullptr && configured[0] == '/')
		{
			output.assign(configured);
			return {};
		}
		return home_child(output, "/.local/share");
	#endif
	}

	Result<void, FileError> native_user_cache_directory(String& output) noexcept
	{
	#if defined(EMBER_PLATFORM_MACOS)
		return home_child(output, "/Library/Caches");
	#else
		const char* configured = std::getenv("XDG_CACHE_HOME");
		if (configured != nullptr && configured[0] == '/')
		{
			output.assign(configured);
			return {};
		}
		return home_child(output, "/.cache");
	#endif
	}

	Result<void, FileError> native_temporary_directory(String& output) noexcept
	{
		const char* configured = std::getenv("TMPDIR");

		output.assign(configured != nullptr && configured[0] == '/' ? configured : "/tmp");

		return {};
	}
}

#endif
