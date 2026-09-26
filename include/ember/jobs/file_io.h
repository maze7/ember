#pragma once

#include <ember/core/filesystem.h>
#include <ember/jobs/job_system.h>

#include <memory_resource>

namespace ember::jobs
{
	/**
	 * A whole file read on the I/O thread. The caller owns it for as long as the read is in flight,
	 * on a parked fiber's stack as a rule, and looks at `result` once the counter completes:
	 *
	 *     FileRead read{.path = path, .memory = &heap};
	 *     Counter done;
	 *
	 *     if (read_file(read, done))
	 *         wait(done);
	 *
	 * The worker the caller was on runs other jobs meanwhile, and the caller resumes wherever a
	 * worker is free, bytes in hand. Several reads may share one counter and one wait: a level's
	 * worth of files lands with one park.
	 */
	struct FileRead
	{
		StringView path					  = {};		 // must outlive the read
		std::pmr::memory_resource* memory = nullptr; // where the bytes go
		fs::ReadFileOptions options		  = {};
		JobPriority priority			  = JobPriority::Normal;

		Result<fs::FileData, fs::FileError> result = {}; // meaningful once the counter completes
	};

	/// Submits the read. A refused submission leaves the counter untouched, as submit_io does.
	[[nodiscard]] inline Result<void, IoSubmitError> read_file(FileRead& read, Counter& done) noexcept
	{
		EMBER_ASSERT(read.memory != nullptr);

		return submit_io(
			{
				.fn =
					[](void* data) noexcept
				{
					FileRead& read = *static_cast<FileRead*>(data);
					read.result	   = fs::read_file(read.path, *read.memory, read.options);
				},
				.data	  = &read,
				.name	  = "read file",
				.priority = read.priority,
			},
			done);
	}

	/**
	 * A whole file written on the I/O thread, all or nothing, with the durability a save needs by
	 * default. The bytes must outlive the write; the caller looks at `result` once the counter
	 * completes, the same way as a read.
	 */
	struct FileWrite
	{
		StringView path				   = {};
		Span<const u8> bytes		   = {};
		fs::WriteDurability durability = fs::WriteDurability::FileDataAndDirectory;
		JobPriority priority		   = JobPriority::Normal;

		Result<void, fs::FileError> result = {}; // meaningful once the counter completes
	};

	/// Submits the write. A refused submission leaves the counter untouched, as submit_io does.
	[[nodiscard]] inline Result<void, IoSubmitError> write_file(FileWrite& write, Counter& done) noexcept
	{
		return submit_io(
			{
				.fn =
					[](void* data) noexcept
				{
					FileWrite& write = *static_cast<FileWrite*>(data);
					write.result	 = fs::write_file_atomic(write.path, write.bytes, write.durability);
				},
				.data	  = &write,
				.name	  = "write file",
				.priority = write.priority,
			},
			done);
	}
}
