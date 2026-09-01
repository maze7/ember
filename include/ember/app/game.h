#pragma once

#include <ember/app/app.h>
#include <ember/app/main.h>

#include <concepts>

namespace ember
{
	/// The shape EMBER_GAME instantiates.
	template <class T>
	concept GameType = std::constructible_from<T, App&> && requires(T& game, App& app, const Args& args) {
		{ T::configure(args) } -> std::same_as<AppDef>;
		{ game.run(app) } -> std::convertible_to<int>;
	};

	/**
	 * Cofigures, boots ember, constructs the game and hands control
	 * to it. Unwinds in reverse order.
	 */
	template <GameType T> int run_game(const Args& args)
	{
		App app(T::configure(args));

		if (!app)
			return 1;

		T game(app);

		if constexpr (requires { static_cast<bool>(game); })
		{
			if (!game)
				return 1;
		}

		return game.run(app);
	}
}

#define EMBER_GAME(T)                                                                                                  \
	static_assert(                                                                                                     \
		ember::GameType<T>,                                                                                            \
		#T " needs: static AppDef configure(const Args&), a T(App&) constructor and int run (App&)");                  \
                                                                                                                       \
	int ember_main(const ember::Args& args) { return ember::run_game<T>(args); }
