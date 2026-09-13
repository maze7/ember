#include <ember/sync/thread.h>

#include <atomic>

#if defined(EMBER_PLATFORM_WINDOWS)
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <windows.h>
#else
	#include <pthread.h>
	#if defined(EMBER_PLATFORM_LINUX)
		#include <sched.h>
	#endif
#endif

namespace ember
{
	u32 current_thread_id()
	{
		static std::atomic<u32> s_id_counter{1};
		// thread_local ensures the atomic is only hit exactly once per thread lifetime.
		thread_local u32 s_tid = s_id_counter.fetch_add(1, std::memory_order_relaxed);
		return s_tid;
	}

	void set_thread_name(const char* name) noexcept
	{
#if defined(EMBER_PLATFORM_WINDOWS)
		wchar_t wide[64];
		size_t length = 0;

		for (; name[length] != 0 && length < 63; ++length)
			wide[length] = static_cast<wchar_t>(name[length]);

		wide[length] = 0;
		SetThreadDescription(GetCurrentThread(), wide);
#elif defined(EMBER_PLATFORM_MACOS)
		pthread_setname_np(name);
#else
		char short_name[16];
		size_t length = 0;

		for (; name[length] != 0 && length < 15; ++length)
			short_name[length] = name[length];

		short_name[length] = 0;
		pthread_setname_np(pthread_self(), short_name);
#endif
	}

	bool set_thread_affinity(u32 core) noexcept
	{
#if defined(EMBER_PLATFORM_WINDOWS)
		if (core >= 64)
			return false;

		return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << core) != 0;
#elif defined(EMBER_PLATFORM_LINUX)
		if (core >= CPU_SETSIZE)
			return false;

		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(core, &set);
		return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
		(void)core;
		return false;
#endif
	}
}
