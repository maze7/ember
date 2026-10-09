#include <ember/core/time.h>

namespace ember
{
	[[nodiscard]] ember::u64 now_ns() noexcept
	{
		return static_cast<ember::u64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
				.count());
	}
}
