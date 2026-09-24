#pragma once

#include <ember/containers/mpmc_queue.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/jobs/job_system.h>

#include <atomic>
#include <memory_resource>
#include <thread>

namespace ember::io
{
	enum class FileError : u8
	{
		None,
		NotFound,	// the open failed
		ReadFailed, // a short read, or the read itself failed
		TooLarge,	// over FileIoDef::max_bytes; a bigger file is a mistake, not a load
		QueueFull,	// more reads in flight than the queue holds; the read never ran
	};

	/// What a read's bytes are aligned to, so a cooked header can be used in place.
	inline constexpr size_t FILE_ALIGNMENT = 16;

	/**
	 * One read, owned by the caller for as long as it is in flight: on a parked fiber's stack is
	 * the usual place. The IO thread fills bytes and error, then signals the counter the read was
	 * submitted with. The bytes come from `memory`, FILE_ALIGNMENT aligned and exactly the file's
	 * size; an empty file reads as no bytes and no error. release() gives them back.
	 */
	struct FileRead
	{
		const char* path				  = nullptr; // must outlive the read
		std::pmr::memory_resource* memory = nullptr; // where the bytes are allocated
		Span<u8> bytes					  = {};
		FileError error					  = FileError::None;

		/// Returns the bytes to `memory`. Nothing to do after a failed or empty read.
		void release() noexcept
		{
			if (!bytes.empty())
				memory->deallocate(bytes.data(), bytes.size(), FILE_ALIGNMENT);

			bytes = {};
		}
	};

	struct FileIoDef
	{
		u32 queue_capacity = 256;	 // reads in flight at once; a power of two
		u64 max_bytes	   = 256_mb; // a larger file fails with TooLarge instead of eating the heap
	};

	/**
	 * The file thread: the one place in the engine that blocks on the operating system. Reads
	 * queue up from any thread, run here one after another, and finish with one jobs::signal()
	 * each, the way Gyrling's I/O threads post a job when the data is in. A loader job submits a
	 * read and parks on its counter; the worker it was on carries on with other jobs, and the
	 * loader resumes wherever a worker is free, bytes in hand.
	 *
	 * There is no cancel: a read always completes, with an error when it must, so shutdown is
	 * "stop submitting, then join". The job system must be up for as long as the thread runs.
	 */
	class FileIo final
	{
	public:
		FileIo() noexcept = default;
		~FileIo() noexcept;

		FileIo(const FileIo&)			 = delete;
		FileIo& operator=(const FileIo&) = delete;

		/// Starts the thread. Once per object, after the memory and job systems are up.
		void init(const FileIoDef& def = {}) noexcept;

		/// Finishes the queued reads and joins the thread. Nothing may submit after this begins.
		void shutdown() noexcept;

		/**
		 * Queues the read. Any thread. `done` was constructed pending (jobs::Counter done{1}) and is
		 * signalled exactly once, on failure too, so `read(); jobs::wait(done);` is the whole protocol.
		 * A full queue, or one that was never started, fails the read at once with QueueFull.
		 */
		void read(FileRead& read, jobs::Counter& done) noexcept;

		/// Reads queued and not yet finished. A hint for stats.
		[[nodiscard]] u32 pending() const noexcept { return static_cast<u32>(m_queue.size_hint()); }

	private:
		/// Queue cells are value initialised, and a nested struct's default member initialisers are
		/// not visible until the enclosing class is complete, so these carry none.
		struct Request
		{
			FileRead* read;
			jobs::Counter* done;
		};

		void wake() noexcept;
		void run() noexcept;
		void perform(FileRead& read) const noexcept;

		MpmcQueue<Request> m_queue{MemoryTag::Assets};
		std::thread m_thread;

		/// Set by every push and by shutdown, cleared by the thread when it wakes: a flag rather
		/// than a semaphore because any number of pushes may land before one wake-up.
		std::atomic<bool> m_signal{false};
		std::atomic<bool> m_stopping{false};
		u64 m_max_bytes = 0;
	};
}
