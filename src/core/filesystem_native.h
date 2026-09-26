#pragma once

#include <ember/core/filesystem.h>

#include <cstdint>

namespace ember::fs::detail
{
	using NativeHandle = uintptr_t;

	inline constexpr NativeHandle INVALID_NATIVE_HANDLE = ~NativeHandle{0};

	[[nodiscard]] Result<NativeHandle, FileError> native_open(StringView path, const OpenOptions& options) noexcept;

	[[nodiscard]] Result<void, FileError> native_close(NativeHandle file) noexcept;

	[[nodiscard]] Result<u64, FileError> native_size(NativeHandle file) noexcept;

	[[nodiscard]] Result<void, FileError> native_resize(NativeHandle file, u64 size) noexcept;

	[[nodiscard]] Result<size_t, FileError> native_read_at(NativeHandle file, u64 offset,
														   Span<u8> destination) noexcept;

	[[nodiscard]] Result<size_t, FileError> native_write_at(NativeHandle file, u64 offset,
															Span<const u8> source) noexcept;

	[[nodiscard]] Result<void, FileError> native_flush(NativeHandle file) noexcept;

	[[nodiscard]] Result<FileInfo, FileError> native_status(StringView path, FollowSymlinks follow_symlinks) noexcept;

	[[nodiscard]] Result<void, FileError> native_enumerate(StringView directory, EnumerateFn visitor,
														   void* data) noexcept;

	[[nodiscard]] Result<void, FileError> native_create_directory(StringView path) noexcept;

	[[nodiscard]] Result<void, FileError> native_remove_file(StringView path) noexcept;

	[[nodiscard]] Result<void, FileError> native_remove_empty_directory(StringView path) noexcept;

	[[nodiscard]] Result<void, FileError> native_remove_tree(StringView path) noexcept;

	[[nodiscard]] Result<void, FileError> native_copy_file(StringView from, StringView to, bool replace) noexcept;

	[[nodiscard]] Result<void, FileError> native_rename(StringView from, StringView to, bool replace) noexcept;

	[[nodiscard]] Result<void, FileError> native_replace(StringView temporary, StringView target,
														 bool sync_parent) noexcept;

	[[nodiscard]] Result<void, FileError> native_sync_parent(StringView path) noexcept;

	[[nodiscard]] u64 native_process_id() noexcept;

	[[nodiscard]] Result<void, FileError> native_working_directory(String& output) noexcept;

	[[nodiscard]] Result<void, FileError> native_executable_directory(String& output) noexcept;

	[[nodiscard]] Result<void, FileError> native_user_data_directory(String& output) noexcept;

	[[nodiscard]] Result<void, FileError> native_user_cache_directory(String& output) noexcept;

	[[nodiscard]] Result<void, FileError> native_temporary_directory(String& output) noexcept;
}
