#pragma once

#include <charconv>
#include <ember/containers/span.h>

namespace ember
{
	/**
	 * One thing off the command line, split at its first '='.
	 * @example "--join=127.0.0.1:30000"
	 */
	struct Arg
	{
		StringView key;	  // "--join": dashes and all so what is typed is what is asked for
		StringView value; // "127.0.0.1:30000"; empty when there was no '=', or nothing after it
	};

	/**
	 * What the platform entry point hands the game via the command line. UTF-8 on every platform,
	 * already split into keys and values. The program's own name is omitted.
	 *
	 * 		--join=127.0.0.1:30000		args.has("--join") == true
	 * 		--join=127.0.0.1:30000		args.string("--join") == "127.0.0.1:30000"
	 * 		--seed=42					args.number<i32>("--seed") == 42
	 *		--fullscreen=false			args.boolean("--fullscreen") == false
	 */
	class Args
	{
	public:
		/** Maximum number of arguments any Ember application supports */
		static constexpr size_t MAX_ARGS = 64;

		constexpr Args() noexcept = default;

		constexpr Args(int argc, const char* const* argv) noexcept
		{
			for (int i = 1; i < argc && m_count < MAX_ARGS; ++i)
			{
				const StringView arg = argv[i];
				const size_t equals	 = arg.find('=');

				m_args[m_count++] = {
					.key   = arg.substr(0, equals),
					.value = equals == StringView::npos ? StringView() : arg.substr(equals + 1),
				};
			}
		}

		/** Every arg, in the order given. */
		constexpr Span<const Arg> all() const noexcept { return {m_args, m_count}; }

		/** The last arg with this key; null when there is none. */
		constexpr const Arg* find(StringView key) const noexcept
		{
			for (size_t i = m_count; i-- > 0;)
				if (m_args[i].key == key)
					return &m_args[i];

			return nullptr;
		}

		/** Whether the key was given, with a value or without. */
		[[nodiscard]] constexpr bool has(StringView key) const noexcept { return find(key) != nullptr; }

		/** The key's value; nullopt when the key is missing or has none. */
		[[nodiscard]] constexpr std::optional<StringView> string(StringView key) const noexcept
		{
			const Arg* arg = find(key);
			if (arg == nullptr || arg->value.empty())
				return std::nullopt;

			return arg->value;
		}

		/** The key's value as a number; nullopt when it is missing, or is not wholly a number that fits T. */
		template <typename T>
			requires std::is_arithmetic_v<T> && (!std::is_same_v<T, bool>)
		[[nodiscard]] std::optional<T> number(StringView key) const noexcept
		{
			const auto text = string(key);
			if (!text)
				return std::nullopt;

			T value{};
			const char* const end = text->data() + text->size();
			const auto parsed	  = std::from_chars(text->data(), end, value);
			if (parsed.ec != std::errc() || parsed.ptr != end)
				return std::nullopt;

			return value;
		}

		/**
		 * The key as a switch: true when it stands alone or says true or 1, false when it says false or 0.
		 * nullopt when it is missing or says anything else.
		 */
		[[nodiscard]] constexpr std::optional<bool> boolean(StringView key) const noexcept
		{
			const Arg* arg = find(key);
			if (arg == nullptr)
				return std::nullopt;

			if (arg->value.empty() || arg->value == "true" || arg->value == "1")
				return true;
			if (arg->value == "false" || arg->value == "0")
				return false;

			return std::nullopt;
		}

	private:
		Arg m_args[MAX_ARGS] = {};
		size_t m_count		 = 0;
	};
}

/** Defined by the game, usually by EMBER_GAME. Owns everything between entry and exit. */
int ember_main(const ember::Args& args);
