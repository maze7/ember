#pragma once

#include <ember/core/common.h>

#include <atomic>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
	#include <immintrin.h>
#endif

namespace ember
{
	namespace detail
	{
		inline void cpu_relax(u32 spin_count) noexcept
		{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
			_mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
			__asm__ __volatile__("yield");
#else
			(void)spin_count;
#endif

			if (spin_count > 64)
				std::this_thread::yield();
		}
	}

	class alignas(EMBER_CACHE_LINE) SpinMutex final
	{
	public:
		SpinMutex() noexcept = default;

		SpinMutex(const SpinMutex&)			   = delete;
		SpinMutex& operator=(const SpinMutex&) = delete;

		[[nodiscard]] bool try_lock() noexcept { return !m_locked.test_and_set(std::memory_order_acquire); }

		void lock() noexcept
		{
			u32 spin_count = 0;

			while (!try_lock())
				detail::cpu_relax(spin_count++);
		}

		void unlock() noexcept { m_locked.clear(std::memory_order_release); }

	private:
		std::atomic_flag m_locked = ATOMIC_FLAG_INIT;
	};
}
