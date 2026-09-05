#pragma once

#include <ember/app/app.h>
#include <ember/app/runtime.h>
#include <ember/core/common.h>

#include <concepts>
#include <type_traits>

namespace ember::detail
{
	// clang-format off
	template <typename T>
	concept ValidApp =
		std::derived_from<T, App> &&
		std::default_initializable<T> &&
		std::is_nothrow_default_constructible_v<T> &&
		requires(const Args& args)
		{
			{ T::configure(args) } noexcept -> std::same_as<AppConfig>;
		};
	// clang-format on

	template <ValidApp T>
	[[nodiscard]] int run_app(const Args& args) noexcept
	{
		const AppConfig config = T::configure(args);

		// This order keeps all engine services alive during App destruction.
		Runtime runtime(config, args);
		if (!runtime)
			return 1;

		T app;
		return runtime.run(app);
	}
}

#define EMBER_APP(Type)                                                                                                \
	static_assert(ember::detail::ValidApp<Type>,                                                                        \
		#Type " must derive from ember::App and be nothrow default constructible");                                    \
                                                                                                                       \
	int ember_main(const ember::Args& args)                                                                             \
	{                                                                                                                   \
		return ember::detail::run_app<Type>(args);                                                                      \
	}
