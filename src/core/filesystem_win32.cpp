#if defined(EMBER_PLATFORM_WINDOWS)

	#include "filesystem_native.h"

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif

	#include <shlobj.h>
	#include <windows.h>

	#include <algorithm>
	#include <cstddef>
	#include <limits>
	#include <string>
	#include <string_view>
	#include <utility>
	#include <vector>

namespace ember::fs::detail
{
	namespace
	{
		constexpr size_t MAX_PATH_CHARS		   = 32'767;
		constexpr size_t DIRECTORY_QUERY_BYTES = 64 * 1024;
		constexpr u32 REPLACE_ATTEMPTS		   = 4;

		constexpr DWORD SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

		// Denying delete sharing pins an opened entry while it is traversed.
		constexpr DWORD SHARE_WHILE_REMOVING = FILE_SHARE_READ | FILE_SHARE_WRITE;

		constexpr u64 WINDOWS_EPOCH_TICKS = 116'444'736'000'000'000ull;

		[[nodiscard]] constexpr FileError error(FileErrorCode code, FileOp op, u32 native = 0) noexcept
		{
			return {.code = code, .op = op, .native_code = native};
		}

		[[nodiscard]] FileError win32_error(DWORD code, FileOp op) noexcept
		{
			FileErrorCode portable = FileErrorCode::Unknown;

			switch (code)
			{
				case ERROR_FILE_NOT_FOUND:
				case ERROR_PATH_NOT_FOUND:
				case ERROR_INVALID_DRIVE:
				case ERROR_BAD_NETPATH:
				case ERROR_BAD_NET_NAME:
					portable = FileErrorCode::NotFound;
					break;

				case ERROR_FILE_EXISTS:
				case ERROR_ALREADY_EXISTS:
					portable = FileErrorCode::AlreadyExists;
					break;

				case ERROR_ACCESS_DENIED:
				case ERROR_PRIVILEGE_NOT_HELD:
					portable = FileErrorCode::AccessDenied;
					break;

				case ERROR_INVALID_PARAMETER:
				case ERROR_INVALID_HANDLE:
					portable = FileErrorCode::InvalidArgument;
					break;

				case ERROR_INVALID_NAME:
				case ERROR_BAD_PATHNAME:
				case ERROR_NO_UNICODE_TRANSLATION:
				case ERROR_CANT_RESOLVE_FILENAME:
					portable = FileErrorCode::InvalidPath;
					break;

				case ERROR_FILENAME_EXCED_RANGE:
				case ERROR_BUFFER_OVERFLOW:
					portable = FileErrorCode::NameTooLong;
					break;

				case ERROR_DIRECTORY:
					portable = FileErrorCode::NotDirectory;
					break;

				case ERROR_DIR_NOT_EMPTY:
					portable = FileErrorCode::DirectoryNotEmpty;
					break;

				case ERROR_DISK_FULL:
				case ERROR_HANDLE_DISK_FULL:
					portable = FileErrorCode::NoSpace;
					break;

				case ERROR_WRITE_PROTECT:
					portable = FileErrorCode::ReadOnly;
					break;

				case ERROR_BUSY:
				case ERROR_SHARING_VIOLATION:
				case ERROR_LOCK_VIOLATION:
				case ERROR_DRIVE_LOCKED:
				case ERROR_CURRENT_DIRECTORY:
				case ERROR_USER_MAPPED_FILE:
				case ERROR_TOO_MANY_OPEN_FILES:
				case ERROR_DELETE_PENDING:
					portable = FileErrorCode::Busy;
					break;

				case ERROR_FILE_TOO_LARGE:
					portable = FileErrorCode::TooLarge;
					break;

				case ERROR_HANDLE_EOF:
					portable = FileErrorCode::UnexpectedEndOfFile;
					break;

				case ERROR_NOT_SAME_DEVICE:
					portable = FileErrorCode::CrossDevice;
					break;

				case ERROR_NOT_SUPPORTED:
				case ERROR_CALL_NOT_IMPLEMENTED:
				case ERROR_INVALID_FUNCTION:
					portable = FileErrorCode::Unsupported;
					break;

				case ERROR_CRC:
				case ERROR_IO_DEVICE:
				case ERROR_READ_FAULT:
				case ERROR_WRITE_FAULT:
				case ERROR_GEN_FAILURE:
					portable = FileErrorCode::Io;
					break;

				default:
					break;
			}

			return error(portable, op, static_cast<u32>(code));
		}

		[[nodiscard]] FileError hresult_error(HRESULT value, FileOp op) noexcept
		{
			FileError result = HRESULT_FACILITY(value) == FACILITY_WIN32 ? win32_error(HRESULT_CODE(value), op)
																		 : error(FileErrorCode::Unknown, op);

			if (value == E_ACCESSDENIED)
				result.code = FileErrorCode::AccessDenied;
			else if (value == E_INVALIDARG)
				result.code = FileErrorCode::InvalidArgument;

			result.native_code = static_cast<u32>(value);
			return result;
		}

		class Handle final
		{
		public:
			Handle() noexcept = default;
			explicit Handle(HANDLE value) noexcept : m_value(value) {}

			~Handle() noexcept
			{
				if (valid())
					(void)CloseHandle(m_value);
			}

			Handle(Handle&& other) noexcept : m_value(std::exchange(other.m_value, INVALID_HANDLE_VALUE)) {}

			Handle& operator=(Handle&& other) noexcept
			{
				if (this != &other)
				{
					if (valid())
						(void)CloseHandle(m_value);

					m_value = std::exchange(other.m_value, INVALID_HANDLE_VALUE);
				}
				return *this;
			}

			Handle(const Handle&)			 = delete;
			Handle& operator=(const Handle&) = delete;

			[[nodiscard]] bool valid() const noexcept { return m_value != nullptr && m_value != INVALID_HANDLE_VALUE; }

			[[nodiscard]] HANDLE get() const noexcept { return m_value; }

			[[nodiscard]] HANDLE release() noexcept { return std::exchange(m_value, INVALID_HANDLE_VALUE); }

			[[nodiscard]] Result<void, FileError> close(FileOp op) noexcept
			{
				if (!valid())
					return {};

				const HANDLE value = release();
				if (!CloseHandle(value))
					return fail(win32_error(GetLastError(), op));

				return {};
			}

		private:
			HANDLE m_value = INVALID_HANDLE_VALUE;
		};

		class FindHandle final
		{
		public:
			explicit FindHandle(HANDLE value) noexcept : m_value(value) {}

			~FindHandle() noexcept
			{
				if (valid())
					(void)FindClose(m_value);
			}

			FindHandle(const FindHandle&)			 = delete;
			FindHandle& operator=(const FindHandle&) = delete;

			[[nodiscard]] bool valid() const noexcept { return m_value != INVALID_HANDLE_VALUE; }

			[[nodiscard]] HANDLE get() const noexcept { return m_value; }

			[[nodiscard]] Result<void, FileError> close() noexcept
			{
				if (!valid())
					return {};

				const HANDLE value = std::exchange(m_value, INVALID_HANDLE_VALUE);

				if (!FindClose(value))
					return fail(win32_error(GetLastError(), FileOp::Enumerate));

				return {};
			}

		private:
			HANDLE m_value;
		};

		class ThreadIoEvent final
		{
		public:
			ThreadIoEvent() noexcept
			{
				m_value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if (m_value == nullptr)
					m_error = GetLastError();
			}

			~ThreadIoEvent() noexcept
			{
				if (m_value != nullptr)
					(void)CloseHandle(m_value);
			}

			ThreadIoEvent(const ThreadIoEvent&)			   = delete;
			ThreadIoEvent& operator=(const ThreadIoEvent&) = delete;

			[[nodiscard]] Result<HANDLE, FileError> prepare(FileOp op) noexcept
			{
				if (m_value == nullptr)
					return fail(win32_error(m_error, op));

				if (!ResetEvent(m_value))
					return fail(win32_error(GetLastError(), op));

				return m_value;
			}

		private:
			HANDLE m_value = nullptr;
			DWORD m_error  = ERROR_SUCCESS;
		};

		/*
		 * Each call waits for completion before returning. A thread can therefore
		 * reuse its event without leaving a pending OVERLAPPED operation behind.
		 */
		thread_local ThreadIoEvent s_io_event;

		[[nodiscard]] HANDLE as_handle(NativeHandle value) noexcept { return reinterpret_cast<HANDLE>(value); }

		[[nodiscard]] NativeHandle as_native(HANDLE value) noexcept { return reinterpret_cast<NativeHandle>(value); }

		[[nodiscard]] constexpr bool ascii_alpha(wchar_t value) noexcept
		{
			return (value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z');
		}

		[[nodiscard]] Result<std::wstring, FileError> native_path(StringView path, FileOp op) noexcept
		{
			if (path.empty())
				return fail(error(FileErrorCode::InvalidPath, op));

			if (path.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
			{
				return fail(error(FileErrorCode::NameTooLong, op));
			}

			for (char value : path)
			{
				// A null would make the OS open less than the validated path.
				if (value == '\0' || value == '\\')
					return fail(error(FileErrorCode::InvalidPath, op));
			}

			const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
												  static_cast<int>(path.size()), nullptr, 0);

			if (count == 0)
				return fail(win32_error(GetLastError(), op));

			std::wstring wide(static_cast<size_t>(count), L'\0');

			if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
									wide.data(), count) != count)
			{
				return fail(win32_error(GetLastError(), op));
			}

			for (wchar_t& value : wide)
				if (value == L'/')
					value = L'\\';

			// Only the backend may introduce Win32 device namespace prefixes.
			if (wide.size() >= 4 && wide[0] == L'\\' && wide[1] == L'\\' && (wide[2] == L'?' || wide[2] == L'.') &&
				wide[3] == L'\\')
			{
				return fail(error(FileErrorCode::InvalidPath, op, ERROR_INVALID_NAME));
			}

			if (wide.size() >= 2 && ascii_alpha(wide[0]) && wide[1] == L':' && (wide.size() < 3 || wide[2] != L'\\'))
			{
				return fail(error(FileErrorCode::InvalidPath, op, ERROR_INVALID_NAME));
			}

			DWORD capacity = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);

			if (capacity == 0)
				return fail(win32_error(GetLastError(), op));

			std::wstring absolute;
			bool resolved = false;

			for (u32 attempt = 0; attempt < 4; ++attempt)
			{
				if (capacity >= MAX_PATH_CHARS)
				{
					return fail(error(FileErrorCode::NameTooLong, op, ERROR_FILENAME_EXCED_RANGE));
				}

				absolute.resize(capacity);

				const DWORD written = GetFullPathNameW(wide.c_str(), capacity, absolute.data(), nullptr);

				if (written == 0)
					return fail(win32_error(GetLastError(), op));

				if (written < capacity)
				{
					absolute.resize(written);
					resolved = true;
					break;
				}

				capacity = written + 1;
			}

			if (!resolved)
				return fail(error(FileErrorCode::Busy, op, ERROR_BUSY));

			std::wstring result;

			if (absolute.size() >= 2 && absolute[0] == L'\\' && absolute[1] == L'\\')
			{
				result = L"\\\\?\\UNC\\";
				result.append(absolute.data() + 2, absolute.size() - 2);
			}
			else if (absolute.size() >= 3 && ascii_alpha(absolute[0]) && absolute[1] == L':' && absolute[2] == L'\\')
			{
				result = L"\\\\?\\";
				result.append(absolute);
			}
			else
			{
				return fail(error(FileErrorCode::InvalidPath, op, ERROR_INVALID_NAME));
			}

			if (result.size() + 1 > MAX_PATH_CHARS)
			{
				return fail(error(FileErrorCode::NameTooLong, op, ERROR_FILENAME_EXCED_RANGE));
			}

			return result;
		}

		[[nodiscard]] Result<std::string, FileError> utf8(std::wstring_view wide, FileOp op) noexcept
		{
			if (wide.empty())
				return std::string{};

			if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
			{
				return fail(error(FileErrorCode::NameTooLong, op));
			}

			const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
												  static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);

			if (count == 0)
				return fail(win32_error(GetLastError(), op));

			std::string result(static_cast<size_t>(count), '\0');

			if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
									result.data(), count, nullptr, nullptr) != count)
			{
				return fail(win32_error(GetLastError(), op));
			}

			return result;
		}

		[[nodiscard]] Result<void, FileError> public_path(std::wstring_view wide, String& output) noexcept
		{
			auto converted = utf8(wide, FileOp::ResolvePath);
			if (!converted)
				return fail(converted.error());

			std::string value = std::move(converted.value());

			for (char& character : value)
				if (character == '\\')
					character = '/';

			if (value.starts_with("//?/UNC/"))
				value.erase(2, 6);
			else if (value.starts_with("//?/"))
				value.erase(0, 4);

			output.assign(value.data(), value.size());

			while (output.size() > 1 && output.back() == '/')
			{
				if (output.size() == 3 && output[1] == ':')
					break;
				output.pop_back();
			}

			return {};
		}

		[[nodiscard]] i64 unix_time_ns(LARGE_INTEGER time) noexcept
		{
			const u64 ticks			= static_cast<u64>(time.QuadPart);
			constexpr u64 MAX_TICKS = static_cast<u64>(std::numeric_limits<i64>::max()) / 100;

			if (ticks >= WINDOWS_EPOCH_TICKS)
			{
				const u64 delta = ticks - WINDOWS_EPOCH_TICKS;
				if (delta > MAX_TICKS)
					return std::numeric_limits<i64>::max();
				return static_cast<i64>(delta * 100);
			}

			const u64 delta = WINDOWS_EPOCH_TICKS - ticks;
			if (delta > MAX_TICKS)
				return std::numeric_limits<i64>::min();

			return -static_cast<i64>(delta * 100);
		}

		[[nodiscard]] Result<DWORD, FileError> attributes(HANDLE handle, FileOp op) noexcept
		{
			FILE_BASIC_INFO info{};

			if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info)))
			{
				return fail(win32_error(GetLastError(), op));
			}

			return info.FileAttributes;
		}

		/*
		 * Public Win32 calls cannot open a named child relative to an already
		 * opened directory handle. These NT calls are isolated here so a missing
		 * export produces Unsupported instead of an unsafe path-based fallback.
		 */
		struct NtUnicodeString
		{
			USHORT length;
			USHORT maximum_length;
			PWSTR buffer;
		};

		struct NtObjectAttributes
		{
			ULONG length;
			HANDLE root_directory;
			NtUnicodeString* object_name;
			ULONG attributes;
			PVOID security_descriptor;
			PVOID security_quality_of_service;
		};

		struct NtIoStatusBlock
		{
			union
			{
				LONG status;
				PVOID pointer;
			};
			ULONG_PTR information;
		};

		struct NtNameEntry
		{
			ULONG next_offset;
			ULONG file_index;
			ULONG name_bytes;
			WCHAR name[1];
		};

		using NtOpenFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, NtObjectAttributes*, NtIoStatusBlock*, ULONG, ULONG);

		using NtQueryDirectoryFileFn = LONG(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, NtIoStatusBlock*, PVOID, ULONG, ULONG,
													BOOLEAN, NtUnicodeString*, BOOLEAN);

		using NtStatusToDosFn = ULONG(WINAPI*)(LONG);

		struct NtApi
		{
			NtOpenFileFn open			 = nullptr;
			NtQueryDirectoryFileFn query = nullptr;
			NtStatusToDosFn to_dos		 = nullptr;

			[[nodiscard]] bool available() const noexcept
			{
				return open != nullptr && query != nullptr && to_dos != nullptr;
			}
		};

		[[nodiscard]] const NtApi& nt_api() noexcept
		{
			static const NtApi api = []() noexcept
			{
				NtApi result{};
				const HMODULE module = GetModuleHandleW(L"ntdll.dll");

				if (module == nullptr)
					return result;

				result.open = reinterpret_cast<NtOpenFileFn>(GetProcAddress(module, "NtOpenFile"));

				result.query = reinterpret_cast<NtQueryDirectoryFileFn>(GetProcAddress(module, "NtQueryDirectoryFile"));

				result.to_dos = reinterpret_cast<NtStatusToDosFn>(GetProcAddress(module, "RtlNtStatusToDosError"));

				return result;
			}();

			return api;
		}

		[[nodiscard]] FileError nt_error(LONG status, FileOp op) noexcept
		{
			const auto& api = nt_api();
			if (api.to_dos == nullptr)
			{
				return error(FileErrorCode::Unknown, op, static_cast<u32>(status));
			}

			const DWORD translated = api.to_dos(status);
			if (translated == ERROR_MR_MID_NOT_FOUND)
			{
				return error(FileErrorCode::Unknown, op, static_cast<u32>(status));
			}

			return win32_error(translated, op);
		}

		[[nodiscard]] Result<Handle, FileError> removal_handle(const std::wstring& path) noexcept
		{
			Handle handle{CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, SHARE_WHILE_REMOVING,
									  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
									  nullptr)};

			if (!handle.valid())
				return fail(win32_error(GetLastError(), FileOp::Remove));

			return handle;
		}

		[[nodiscard]] Result<void, FileError> delete_handle(Handle& handle) noexcept
		{
			FILE_DISPOSITION_INFO disposition{};
			disposition.DeleteFile = TRUE;

			if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
			{
				return fail(win32_error(GetLastError(), FileOp::Remove));
			}

			return handle.close(FileOp::Remove);
		}

		struct RemoveFrame
		{
			Handle object;
			Handle enumeration;
			bool directory = false;
		};

		[[nodiscard]] Result<RemoveFrame, FileError> make_frame(Handle object) noexcept
		{
			auto found = attributes(object.get(), FileOp::Remove);
			if (!found)
				return fail(found.error());

			RemoveFrame frame{};
			frame.directory =
				(found.value() & FILE_ATTRIBUTE_DIRECTORY) != 0 && (found.value() & FILE_ATTRIBUTE_REPARSE_POINT) == 0;

			frame.object = std::move(object);

			if (!frame.directory)
				return frame;

			Handle enumeration{ReOpenFile(frame.object.get(),
										  FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
										  SHARE_ALL, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)};

			if (!enumeration.valid())
				return fail(win32_error(GetLastError(), FileOp::Remove));

			frame.enumeration = std::move(enumeration);
			return frame;
		}

		[[nodiscard]] Result<RemoveFrame, FileError> child_frame(HANDLE parent, std::wstring_view child) noexcept
		{
			const size_t byte_count = child.size() * sizeof(wchar_t);

			if (byte_count > std::numeric_limits<USHORT>::max())
			{
				return fail(error(FileErrorCode::NameTooLong, FileOp::Remove, ERROR_FILENAME_EXCED_RANGE));
			}

			NtUnicodeString name{static_cast<USHORT>(byte_count), static_cast<USHORT>(byte_count),
								 const_cast<PWSTR>(child.data())};

			NtObjectAttributes attributes{sizeof(NtObjectAttributes),
										  parent,
										  &name,
										  0x40, // OBJ_CASE_INSENSITIVE
										  nullptr,
										  nullptr};

			NtIoStatusBlock io{};
			HANDLE raw = INVALID_HANDLE_VALUE;

			const ULONG options = 0x20 |	// FILE_SYNCHRONOUS_IO_NONALERT
								  0x4000 |	// FILE_OPEN_FOR_BACKUP_INTENT
								  0x200000; // FILE_OPEN_REPARSE_POINT

			const LONG status = nt_api().open(&raw, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &io,
											  SHARE_WHILE_REMOVING, options);

			if (status < 0)
				return fail(nt_error(status, FileOp::Remove));

			return make_frame(Handle{raw});
		}

		[[nodiscard]] Result<bool, FileError> first_child(HANDLE directory, std::vector<std::byte>& buffer,
														  std::wstring& name) noexcept
		{
			constexpr LONG STATUS_NO_MORE_FILES_VALUE = static_cast<LONG>(0x80000006u);

			constexpr LONG STATUS_NO_SUCH_FILE_VALUE = static_cast<LONG>(0xC000000Fu);

			BOOLEAN restart = TRUE;

			for (;;)
			{
				NtIoStatusBlock io{};

				LONG status = nt_api().query(directory, nullptr, nullptr, nullptr, &io, buffer.data(),
											 static_cast<ULONG>(buffer.size()),
											 12, // FileNamesInformation
											 TRUE, nullptr, restart);

				restart = FALSE;

				if (status == STATUS_NO_MORE_FILES_VALUE || status == STATUS_NO_SUCH_FILE_VALUE)
				{
					return false;
				}

				if (status < 0)
					return fail(nt_error(status, FileOp::Remove));

				if (io.information < offsetof(NtNameEntry, name))
				{
					return fail(error(FileErrorCode::Io, FileOp::Remove, ERROR_INVALID_DATA));
				}

				const auto* entry = reinterpret_cast<const NtNameEntry*>(buffer.data());

				if ((entry->name_bytes % sizeof(wchar_t)) != 0 ||
					offsetof(NtNameEntry, name) + entry->name_bytes > io.information)
				{
					return fail(error(FileErrorCode::Io, FileOp::Remove, ERROR_INVALID_DATA));
				}

				const std::wstring_view child{entry->name, entry->name_bytes / sizeof(wchar_t)};

				if (child == L"." || child == L"..")
					continue;

				name.assign(child.data(), child.size());
				return true;
			}
		}

		[[nodiscard]] Result<void, FileError> remove_leaf(StringView path, bool require_directory) noexcept
		{
			auto converted = native_path(path, FileOp::Remove);
			if (!converted)
				return fail(converted.error());

			auto opened = removal_handle(converted.value());
			if (!opened)
				return fail(opened.error());

			Handle handle = std::move(opened.value());
			auto found	  = attributes(handle.get(), FileOp::Remove);
			if (!found)
				return fail(found.error());

			const bool directory =
				(found.value() & FILE_ATTRIBUTE_DIRECTORY) != 0 && (found.value() & FILE_ATTRIBUTE_REPARSE_POINT) == 0;

			if (directory != require_directory)
			{
				return fail(error(require_directory ? FileErrorCode::NotDirectory : FileErrorCode::IsDirectory,
								  FileOp::Remove, ERROR_DIRECTORY));
			}

			return delete_handle(handle);
		}

		[[nodiscard]] Result<void, FileError> known_folder(REFKNOWNFOLDERID folder, String& output) noexcept
		{
			PWSTR path			 = nullptr;
			const HRESULT result = SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &path);

			if (FAILED(result))
				return fail(hresult_error(result, FileOp::ResolvePath));

			const std::wstring_view value{path};
			auto assigned = public_path(value, output);
			CoTaskMemFree(path);
			return assigned;
		}
	}

	Result<NativeHandle, FileError> native_open(StringView path, const OpenOptions& options) noexcept
	{
		auto converted = native_path(path, FileOp::Open);
		if (!converted)
			return fail(converted.error());

		DWORD access  = FILE_READ_ATTRIBUTES;
		bool writable = false;

		switch (options.access)
		{
			case Access::Read:
				access |= GENERIC_READ;
				break;
			case Access::Write:
				access |= GENERIC_WRITE;
				writable = true;
				break;
			case Access::ReadWrite:
				access |= GENERIC_READ | GENERIC_WRITE;
				writable = true;
				break;
			default:
				return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		if (options.create != Create::OpenExisting && !writable)
		{
			return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		const bool no_follow = options.follow_symlinks == FollowSymlinks::No;

		if (!no_follow && options.follow_symlinks != FollowSymlinks::Yes)
		{
			return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		DWORD creation			  = 0;
		bool truncate_after_check = false;

		switch (options.create)
		{
			case Create::OpenExisting:
				creation = OPEN_EXISTING;
				break;
			case Create::CreateNew:
				creation = CREATE_NEW;
				break;
			case Create::OpenOrCreat:
				creation = OPEN_ALWAYS;
				break;
			case Create::CreateAlways:
				creation			 = no_follow ? OPEN_ALWAYS : CREATE_ALWAYS;
				truncate_after_check = no_follow;
				break;
			case Create::TruncateExisting:
				creation			 = no_follow ? OPEN_EXISTING : TRUNCATE_EXISTING;
				truncate_after_check = no_follow;
				break;
			default:
				return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		const u8 share = static_cast<u8>(options.share);
		constexpr u8 known_share =
			static_cast<u8>(FileShare::Read) | static_cast<u8>(FileShare::Write) | static_cast<u8>(FileShare::Delete);

		if ((share & ~known_share) != 0)
		{
			return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		DWORD sharing = 0;
		if ((share & static_cast<u8>(FileShare::Read)) != 0)
			sharing |= FILE_SHARE_READ;
		if ((share & static_cast<u8>(FileShare::Write)) != 0)
			sharing |= FILE_SHARE_WRITE;
		if ((share & static_cast<u8>(FileShare::Delete)) != 0)
			sharing |= FILE_SHARE_DELETE;

		DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED;

		switch (options.usage)
		{
			case Usage::Normal:
				break;
			case Usage::Sequential:
				flags |= FILE_FLAG_SEQUENTIAL_SCAN;
				break;
			case Usage::Random:
				flags |= FILE_FLAG_RANDOM_ACCESS;
				break;
			default:
				return fail(error(FileErrorCode::InvalidArgument, FileOp::Open));
		}

		if (no_follow && options.create != Create::CreateNew)
			flags |= FILE_FLAG_OPEN_REPARSE_POINT;

		Handle handle{CreateFileW(converted->c_str(), access, sharing, nullptr, creation, flags, nullptr)};

		if (!handle.valid())
			return fail(win32_error(GetLastError(), FileOp::Open));

		if (no_follow)
		{
			FILE_ATTRIBUTE_TAG_INFO info{};

			if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info, sizeof(info)))
			{
				return fail(win32_error(GetLastError(), FileOp::Open));
			}

			if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			{
				return fail(error(FileErrorCode::InvalidPath, FileOp::Open, ERROR_CANT_ACCESS_FILE));
			}
		}

		if (truncate_after_check)
		{
			FILE_END_OF_FILE_INFO end{};

			if (!SetFileInformationByHandle(handle.get(), FileEndOfFileInfo, &end, sizeof(end)))
			{
				return fail(win32_error(GetLastError(), FileOp::Open));
			}
		}

		return as_native(handle.release());
	}

	Result<void, FileError> native_close(NativeHandle file) noexcept
	{
		Handle handle{as_handle(file)};
		return handle.close(FileOp::Close);
	}

	Result<u64, FileError> native_size(NativeHandle file) noexcept
	{
		LARGE_INTEGER size{};

		if (!GetFileSizeEx(as_handle(file), &size))
			return fail(win32_error(GetLastError(), FileOp::Stat));

		if (size.QuadPart < 0)
			return fail(error(FileErrorCode::Io, FileOp::Stat));

		return static_cast<u64>(size.QuadPart);
	}

	Result<void, FileError> native_resize(NativeHandle file, u64 size) noexcept
	{
		if (size > static_cast<u64>(std::numeric_limits<LONGLONG>::max()))
		{
			return fail(error(FileErrorCode::TooLarge, FileOp::Resize));
		}

		FILE_END_OF_FILE_INFO end{};
		end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);

		if (!SetFileInformationByHandle(as_handle(file), FileEndOfFileInfo, &end, sizeof(end)))
		{
			return fail(win32_error(GetLastError(), FileOp::Resize));
		}

		return {};
	}

	Result<size_t, FileError> native_read_at(NativeHandle file, u64 offset, Span<u8> destination) noexcept
	{
		const u64 maximum = static_cast<u64>(std::numeric_limits<LONGLONG>::max());

		if (offset > maximum)
			return fail(error(FileErrorCode::TooLarge, FileOp::Read));

		const size_t count = static_cast<size_t>(
			std::min<u64>(destination.size(), std::min<u64>(std::numeric_limits<DWORD>::max(), maximum - offset + 1)));

		if (count == 0)
			return size_t{0};

		auto event = s_io_event.prepare(FileOp::Read);
		if (!event)
			return fail(event.error());

		OVERLAPPED operation{};
		operation.Offset	 = static_cast<DWORD>(offset);
		operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
		operation.hEvent	 = event.value();

		const HANDLE handle = as_handle(file);
		const BOOL started	= ReadFile(handle, destination.data(), static_cast<DWORD>(count), nullptr, &operation);

		if (!started)
		{
			const DWORD code = GetLastError();
			if (code == ERROR_HANDLE_EOF)
				return size_t{0};
			if (code != ERROR_IO_PENDING)
				return fail(win32_error(code, FileOp::Read));
		}

		DWORD transferred = 0;
		if (!GetOverlappedResult(handle, &operation, &transferred, TRUE))
		{
			const DWORD code = GetLastError();
			if (code == ERROR_HANDLE_EOF)
				return size_t{0};
			return fail(win32_error(code, FileOp::Read));
		}

		return static_cast<size_t>(transferred);
	}

	Result<size_t, FileError> native_write_at(NativeHandle file, u64 offset, Span<const u8> source) noexcept
	{
		const u64 maximum = static_cast<u64>(std::numeric_limits<LONGLONG>::max());

		if (offset > maximum)
			return fail(error(FileErrorCode::TooLarge, FileOp::Write));

		const size_t count = static_cast<size_t>(
			std::min<u64>(source.size(), std::min<u64>(std::numeric_limits<DWORD>::max(), maximum - offset + 1)));

		if (count == 0)
			return size_t{0};

		auto event = s_io_event.prepare(FileOp::Write);
		if (!event)
			return fail(event.error());

		OVERLAPPED operation{};
		operation.Offset	 = static_cast<DWORD>(offset);
		operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
		operation.hEvent	 = event.value();

		const HANDLE handle = as_handle(file);
		const BOOL started	= WriteFile(handle, source.data(), static_cast<DWORD>(count), nullptr, &operation);

		if (!started && GetLastError() != ERROR_IO_PENDING)
			return fail(win32_error(GetLastError(), FileOp::Write));

		DWORD transferred = 0;
		if (!GetOverlappedResult(handle, &operation, &transferred, TRUE))
		{
			return fail(win32_error(GetLastError(), FileOp::Write));
		}

		return static_cast<size_t>(transferred);
	}

	Result<void, FileError> native_flush(NativeHandle file) noexcept
	{
		if (!FlushFileBuffers(as_handle(file)))
			return fail(win32_error(GetLastError(), FileOp::Flush));
		return {};
	}

	Result<FileInfo, FileError> native_status(StringView path, FollowSymlinks follow) noexcept
	{
		auto converted = native_path(path, FileOp::Stat);
		if (!converted)
			return fail(converted.error());

		DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
		if (follow == FollowSymlinks::No)
			flags |= FILE_FLAG_OPEN_REPARSE_POINT;
		else if (follow != FollowSymlinks::Yes)
			return fail(error(FileErrorCode::InvalidArgument, FileOp::Stat));

		Handle handle{
			CreateFileW(converted->c_str(), FILE_READ_ATTRIBUTES, SHARE_ALL, nullptr, OPEN_EXISTING, flags, nullptr)};

		if (!handle.valid())
			return fail(win32_error(GetLastError(), FileOp::Stat));

		FILE_BASIC_INFO basic{};
		FILE_STANDARD_INFO standard{};

		if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)) ||
			!GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard)))
		{
			return fail(win32_error(GetLastError(), FileOp::Stat));
		}

		if (standard.EndOfFile.QuadPart < 0)
			return fail(error(FileErrorCode::Io, FileOp::Stat));

		FileType type = FileType::Other;

		if (follow == FollowSymlinks::No && (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		{
			type = FileType::Symlink;
		}
		else if ((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			type = FileType::Directory;
		}
		else if (GetFileType(handle.get()) == FILE_TYPE_DISK)
		{
			type = FileType::Regular;
		}

		return FileInfo{.type = type,
						.size = type == FileType::Directory ? 0 : static_cast<u64>(standard.EndOfFile.QuadPart),
						.modified_time_ns = unix_time_ns(basic.LastWriteTime)};
	}

	Result<void, FileError> native_enumerate(StringView directory, EnumerateFn visitor, void* data) noexcept
	{
		auto converted = native_path(directory, FileOp::Enumerate);
		if (!converted)
			return fail(converted.error());

		std::wstring search = std::move(converted.value());
		if (search.back() != L'\\')
			search.push_back(L'\\');
		search.push_back(L'*');

		WIN32_FIND_DATAW item{};

		HANDLE raw = FindFirstFileExW(search.c_str(), FindExInfoBasic, &item, FindExSearchNameMatch, nullptr,
									  FIND_FIRST_EX_LARGE_FETCH);

		if (raw == INVALID_HANDLE_VALUE)
		{
			const DWORD code = GetLastError();
			if (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED)
			{
				raw = FindFirstFileExW(search.c_str(), FindExInfoBasic, &item, FindExSearchNameMatch, nullptr, 0);
			}
		}

		FindHandle find{raw};

		if (!find.valid())
		{
			const DWORD code = GetLastError();

			if (code == ERROR_FILE_NOT_FOUND)
			{
				auto info = native_status(directory, FollowSymlinks::Yes);

				if (!info)
					return fail(info.error());

				if (info->type == FileType::Directory)
					return {};

				return fail(error(FileErrorCode::NotDirectory, FileOp::Enumerate, ERROR_DIRECTORY));
			}

			return fail(win32_error(code, FileOp::Enumerate));
		}

		for (;;)
		{
			const std::wstring_view name{item.cFileName};

			if (name != L"." && name != L"..")
			{
				auto converted_name = utf8(name, FileOp::Enumerate);
				if (!converted_name)
					return fail(converted_name.error());

				FileType type = FileType::Regular;

				if ((item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
					type = FileType::Symlink;
				else if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
					type = FileType::Directory;
				else if ((item.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0)
					type = FileType::Other;

				const DirectoryEntry entry{.name = StringView{converted_name.value()}, .type = type};

				if (visitor(entry, data) == Visit::Stop)
					return find.close();
			}

			if (!FindNextFileW(find.get(), &item))
			{
				const DWORD code = GetLastError();
				if (code != ERROR_NO_MORE_FILES)
					return fail(win32_error(code, FileOp::Enumerate));

				return find.close();
			}
		}
	}

	Result<void, FileError> native_create_directory(StringView path) noexcept
	{
		auto converted = native_path(path, FileOp::CreateDirectory);
		if (!converted)
			return fail(converted.error());

		if (CreateDirectoryW(converted->c_str(), nullptr))
			return {};

		const DWORD code = GetLastError();

		if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)
		{
			const DWORD attributes = GetFileAttributesW(converted->c_str());

			if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				return {};
			}
		}

		return fail(win32_error(code, FileOp::CreateDirectory));
	}

	Result<void, FileError> native_remove_file(StringView path) noexcept { return remove_leaf(path, false); }

	Result<void, FileError> native_remove_empty_directory(StringView path) noexcept { return remove_leaf(path, true); }

	Result<void, FileError> native_remove_tree(StringView path) noexcept
	{
		if (!nt_api().available())
		{
			return fail(error(FileErrorCode::Unsupported, FileOp::Remove, ERROR_CALL_NOT_IMPLEMENTED));
		}

		auto converted = native_path(path, FileOp::Remove);
		if (!converted)
			return fail(converted.error());

		auto opened = removal_handle(converted.value());
		if (!opened)
			return fail(opened.error());

		auto root = make_frame(std::move(opened.value()));
		if (!root)
			return fail(root.error());

		std::vector<RemoveFrame> frames;
		frames.push_back(std::move(root.value()));

		std::vector<std::byte> buffer(DIRECTORY_QUERY_BYTES);
		std::wstring child;

		while (!frames.empty())
		{
			RemoveFrame& frame = frames.back();

			if (frame.directory)
			{
				auto found = first_child(frame.enumeration.get(), buffer, child);

				if (!found)
					return fail(found.error());

				if (found.value())
				{
					auto opened_child = child_frame(frame.enumeration.get(), child);

					if (!opened_child)
					{
						if (opened_child.error().code == FileErrorCode::NotFound)
						{
							continue;
						}
						return fail(opened_child.error());
					}

					frames.push_back(std::move(opened_child.value()));
					continue;
				}

				auto closed = frame.enumeration.close(FileOp::Remove);
				if (!closed)
					return fail(closed.error());
			}

			auto removed = delete_handle(frame.object);
			if (!removed)
				return fail(removed.error());

			frames.pop_back();
		}

		return {};
	}

	Result<void, FileError> native_copy_file(StringView from, StringView to, bool replace) noexcept
	{
		auto source = native_path(from, FileOp::Copy);
		if (!source)
			return fail(source.error());

		auto destination = native_path(to, FileOp::Copy);
		if (!destination)
			return fail(destination.error());

		if (!CopyFileW(source->c_str(), destination->c_str(), replace ? FALSE : TRUE))
		{
			return fail(win32_error(GetLastError(), FileOp::Copy));
		}

		return {};
	}

	Result<void, FileError> native_rename(StringView from, StringView to, bool replace) noexcept
	{
		auto source = native_path(from, FileOp::Rename);
		if (!source)
			return fail(source.error());

		auto destination = native_path(to, FileOp::Rename);
		if (!destination)
			return fail(destination.error());

		if (!MoveFileExW(source->c_str(), destination->c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0))
		{
			return fail(win32_error(GetLastError(), FileOp::Rename));
		}

		return {};
	}

	Result<void, FileError> native_replace(StringView temporary, StringView target, bool sync_parent) noexcept
	{
		// Reject an unavailable guarantee before changing the destination.
		if (sync_parent)
		{
			return fail(error(FileErrorCode::Unsupported, FileOp::Replace, ERROR_NOT_SUPPORTED));
		}

		auto source = native_path(temporary, FileOp::Replace);
		if (!source)
			return fail(source.error());

		auto destination = native_path(target, FileOp::Replace);
		if (!destination)
			return fail(destination.error());

		for (u32 attempt = 0; attempt < REPLACE_ATTEMPTS; ++attempt)
		{
			if (ReplaceFileW(destination->c_str(), source->c_str(), nullptr, 0, nullptr, nullptr))
			{
				return {};
			}

			const DWORD replace_error = GetLastError();
			if (replace_error != ERROR_FILE_NOT_FOUND)
				return fail(win32_error(replace_error, FileOp::Replace));

			// The target was absent. Do not overwrite a racing creator here.
			if (MoveFileExW(source->c_str(), destination->c_str(), 0))
			{
				return {};
			}

			const DWORD move_error = GetLastError();
			if (move_error != ERROR_FILE_EXISTS && move_error != ERROR_ALREADY_EXISTS)
			{
				return fail(win32_error(move_error, FileOp::Replace));
			}
		}

		return fail(error(FileErrorCode::Busy, FileOp::Replace, ERROR_BUSY));
	}

	Result<void, FileError> native_sync_parent(StringView) noexcept
	{
		return fail(error(FileErrorCode::Unsupported, FileOp::Flush, ERROR_NOT_SUPPORTED));
	}

	u64 native_process_id() noexcept { return static_cast<u64>(GetCurrentProcessId()); }

	Result<void, FileError> native_working_directory(String& output) noexcept
	{
		std::vector<wchar_t> buffer(512);

		for (;;)
		{
			const DWORD count = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());

			if (count == 0)
				return fail(win32_error(GetLastError(), FileOp::ResolvePath));

			if (count < buffer.size())
				return public_path({buffer.data(), count}, output);

			if (count >= MAX_PATH_CHARS)
				return fail(error(FileErrorCode::NameTooLong, FileOp::ResolvePath));

			buffer.resize(static_cast<size_t>(count) + 1);
		}
	}

	Result<void, FileError> native_executable_directory(String& output) noexcept
	{
		std::vector<wchar_t> buffer(512);

		for (;;)
		{
			const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

			if (count == 0)
				return fail(win32_error(GetLastError(), FileOp::ResolvePath));

			if (count < buffer.size())
			{
				const std::wstring_view path{buffer.data(), count};
				const size_t slash = path.find_last_of(L"\\/");

				if (slash == std::wstring_view::npos)
				{
					return fail(error(FileErrorCode::InvalidPath, FileOp::ResolvePath));
				}

				size_t length = slash;
				if (slash == 0)
					length = 1;
				else if (slash == 2 && path[1] == L':')
					length = 3;

				return public_path(path.substr(0, length), output);
			}

			if (buffer.size() >= MAX_PATH_CHARS)
			{
				return fail(error(FileErrorCode::NameTooLong, FileOp::ResolvePath));
			}

			buffer.resize(std::min(buffer.size() * 2, MAX_PATH_CHARS));
		}
	}

	Result<void, FileError> native_user_data_directory(String& output) noexcept
	{
		return known_folder(FOLDERID_RoamingAppData, output);
	}

	Result<void, FileError> native_user_cache_directory(String& output) noexcept
	{
		return known_folder(FOLDERID_LocalAppData, output);
	}

	Result<void, FileError> native_temporary_directory(String& output) noexcept
	{
		std::vector<wchar_t> buffer(512);

		for (;;)
		{
			const DWORD count = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());

			if (count == 0)
				return fail(win32_error(GetLastError(), FileOp::ResolvePath));

			if (count < buffer.size())
				return public_path({buffer.data(), count}, output);

			if (count >= MAX_PATH_CHARS)
			{
				return fail(error(FileErrorCode::NameTooLong, FileOp::ResolvePath));
			}

			buffer.resize(static_cast<size_t>(count) + 1);
		}
	}
}

#endif
