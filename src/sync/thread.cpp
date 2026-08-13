#include <atomic>
#include <ember/sync/thread.h>

namespace ember
{
	u32 current_thread_id()
	{
		static std::atomic<u32> s_id_counter{1};
		// thread_local ensures the atomic is only hit exactly once per thread lifetime.
		thread_local u32 s_tid = s_id_counter.fetch_add(1, std::memory_order_relaxed);
		return s_tid;
	}
}
