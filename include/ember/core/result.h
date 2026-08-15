#pragma once

#include <tl/expected.hpp>
#include <type_traits>
#include <utility>

namespace ember
{
	template <typename T, typename E>
	using Result = tl::expected<T, E>;

	template <typename E>
	[[nodiscard]] constexpr auto fail(E&& error)
	{
		using Error = std::decay_t<E>;
		return tl::unexpected<Error>(std::forward<E>(error));
	}
}
