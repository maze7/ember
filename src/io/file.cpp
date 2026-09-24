#include <ember/core/bits.h>
#include <ember/core/logger.h>
#include <ember/io/file.h>
#include <ember/memory/memory.h>
#include <ember/sync/spin_mutex.h>
#include <ember/sync/thread.h>

#include <cstdio>

namespace ember::io
{
	FileIo::~FileIo() noexcept { shutdown(); }

	void FileIo::init(const FileIoDef& def) noexcept
	{
		EMBER_ASSERT(!m_thread.joinable() && "init runs once");
		EMBER_ASSERT(def.queue_capacity >= 2 && is_power_of_two(def.queue_capacity));

		m_max_bytes = def.max_bytes;
		m_queue.init(def.queue_capacity);
		m_thread = std::thread([this] { run(); });
	}

	void FileIo::shutdown() noexcept
	{
		if (!m_thread.joinable())
			return;

		// The thread drains what is queued before it looks at the flag, so every read submitted
		// before this point still completes and signals.
		m_stopping.store(true, std::memory_order_release);
		wake();
		m_thread.join();
	}

	void FileIo::read(FileRead& read, jobs::Counter& done) noexcept
	{
		EMBER_ASSERT(read.path != nullptr && read.memory != nullptr);
		EMBER_ASSERT(!m_stopping.load(std::memory_order_relaxed) && "read after shutdown began");

		read.bytes = {};
		read.error = FileError::None;

		// A push can be refused while the thread is mid pop one lap behind; that side always
		// finishes, so only a queue with no room turns the read away. The room check comes
		// first because a queue that was never started has no cells to try.
		for (u32 spins = 0;; ++spins)
		{
			if (m_queue.size_hint() >= m_queue.capacity())
			{
				read.error = FileError::QueueFull;
				jobs::signal(done); // the protocol holds on every path: exactly one signal
				return;
			}

			if (m_queue.try_push({&read, &done}))
				break;

			detail::cpu_relax(spins);
		}

		wake();
	}

	void FileIo::wake() noexcept
	{
		m_signal.store(true, std::memory_order_release);
		m_signal.notify_one();
	}

	void FileIo::run() noexcept
	{
		// The allocator wants every thread announced, though the bytes come from the caller's resource.
		memory::initialize_thread();
		set_thread_name("ember io");

		for (;;)
		{
			Request request;

			while (m_queue.try_pop(request))
			{
				perform(*request.read);
				jobs::signal(*request.done); // the loader resumes on whichever worker is free
			}

			if (m_stopping.load(std::memory_order_acquire))
				break;

			// Sleep until a push announces itself. A push that landed during the drain has set the
			// flag already, so this returns at once; clearing it before the next drain means a push
			// that lands in between is drained now and costs a spare wake-up at worst.
			m_signal.wait(false, std::memory_order_acquire);
			m_signal.store(false, std::memory_order_relaxed);
		}

		memory::shutdown_thread();
	}

	void FileIo::perform(FileRead& read) const noexcept
	{
		std::FILE* file = std::fopen(read.path, "rb");

		if (file == nullptr)
		{
			read.error = FileError::NotFound;
			return;
		}

		// ftell is a long: 64 bits everywhere but Windows, where a file that overflows it is over
		// the limit anyway and fails the same way.
		std::fseek(file, 0, SEEK_END);
		const long size = std::ftell(file);
		std::rewind(file);

		if (size < 0 || static_cast<u64>(size) > m_max_bytes)
		{
			read.error = size < 0 ? FileError::ReadFailed : FileError::TooLarge;
			std::fclose(file);
			return;
		}

		// An empty file is a successful read of nothing: no allocation, nothing to release.
		if (size == 0)
		{
			std::fclose(file);
			return;
		}

		const size_t bytes = static_cast<size_t>(size);
		u8* data		   = static_cast<u8*>(read.memory->allocate(bytes, FILE_ALIGNMENT));
		const size_t got   = std::fread(data, 1, bytes, file);
		std::fclose(file);

		if (got != bytes)
		{
			read.memory->deallocate(data, bytes, FILE_ALIGNMENT);
			read.error = FileError::ReadFailed;
			return;
		}

		read.bytes = {data, bytes};
	}
}
