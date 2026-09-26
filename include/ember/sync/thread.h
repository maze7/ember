#pragma once

#include <ember/core/common.h>

namespace ember
{
	/**
	 * Much faster than OS thread IDs and perfect for array indexing in thread-local allocators.
	 *
	 * Never inlined: a job resumes on whichever worker picks it up, so a thread local read the
	 * optimiser hoisted out of a wait would hand back the thread the job started on.
	 */
	EMBER_NOINLINE u32 current_thread_id();

	/// Names the calling thread for debuggers and profilers. Linux keeps 15 characters.
	void set_thread_name(const char* name) noexcept;

	/// Pins the calling thread to one logical core. False where unsupported or refused.
	bool set_thread_affinity(u32 core) noexcept;

	/// What a thread the engine made is for. The kind sets what it may do with the scheduler.
	enum class ThreadKind : u8
	{
		Worker, // runs jobs on fibers, and may wait
		Io,		// blocks on the outside world and reports back; never waits on the scheduler
	};

	/**
	 * The calling OS thread, attached to the engine for as long as this lives: the allocators
	 * are told about it, it claims a frame memory slot, an dit is named for debuggers and the
	 * porfiler. Every thread the engine makes begins with one, the workers, the I/O therad, a
	 * network thread. Detaches on the way out, in reverse. A thread that finds no frame memory
	 * slot ends the process, as an allocation failure would.
	 */
	class ThreadAttachment final
	{
	public:
		ThreadAttachment(const char* name, ThreadKind kind) noexcept;
		~ThreadAttachment() noexcept;

		ThreadAttachment(const ThreadAttachment&)			 = delete;
		ThreadAttachment& operator=(const ThreadAttachment&) = delete;
	};

	/**
	 * True on a thread attached as ThreadKind::Io: one that serves the scheduler from outside
	 * and must never wait on it, since nothing would serve its own queue meanwhile. Not inlined,
	 * for the reason current_thread_id() is not.
	 */
	EMBER_NOINLINE bool is_io_thread() noexcept;
	EMBER_NOINLINE bool is_worker_thread() noexcept;

#if EMBER_LOCK_TRACKING
	/**
	 * Debug count of the spin locks the calling thread holds. A spin lock is taken and released
	 * on one thread with no wait between, so this is useful to make sure a fiber doesn't own
	 * any locks when switching.
	 */
	EMBER_NOINLINE void note_lock_taken() noexcept;
	EMBER_NOINLINE void note_lock_released() noexcept;
	EMBER_NOINLINE u32 locks_held() noexcept;

	#define EMBER_LOCK_TAKEN() ::ember::note_lock_taken()
	#define EMBER_LOCK_RELEASED() ::ember::note_lock_released()
#else
	#define EMBER_LOCK_TAKEN() ((void)0)
	#define EMBER_LOCK_RELEASED() ((void)0)
#endif
}
