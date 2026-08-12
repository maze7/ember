#pragma once

#include <ember/core/common.h>
#include <fmt/format.h>

#include <chrono>
#include <mutex>
#include <source_location>

/**
 * Compile-time log level stripping. Define EMBER_LOG_LEVEL before including this header
 * to strip lower-severity logs from builds entirely.
 *
 * 0 = all,
 * 1 = info+,
 * 2 = warn+,
 * 3 = error only,
 * 4 = silent
 */
#ifndef EMBER_LOG_LEVEL
	#define EMBER_LOG_LEVEL 0
#endif

#if EMBER_LOG_LEVEL <= 0
	#define EMBER_TRACE(...) ember::Logger::trace(std::source_location::current(), __VA_ARGS__)
#else
	#define EMBER_TRACE(...) ((void)0)
#endif

#if EMBER_LOG_LEVEL <= 1
	#define EMBER_INFO(...) ember::Logger::info(std::source_location::current(), __VA_ARGS__)
#else
	#define EMBER_INFO(...) ((void)0)
#endif

#if EMBER_LOG_LEVEL <= 2
	#define EMBER_WARN(...) ember::Logger::warn(std::source_location::current(), __VA_ARGS__)
#else
	#define EMBER_WARN(...) ((void)0)
#endif

#if EMBER_LOG_LEVEL <= 3
	#define EMBER_ERROR(...) ember::Logger::error(std::source_location::current(), __VA_ARGS__)
#else
	#define EMBER_ERROR(...) ((void)0)
#endif

namespace ember
{
	enum class LogLevel : u8
	{
		Trace  = 0,
		Info   = 1,
		Warn   = 2,
		Error  = 3,
		Silent = 4
	};

	/**
	 * Very naive logger implementation for now while we get the rest of the engine off the ground.
	 * This should eventually expand to lock-free log submission from multiple threads, multiple sinks
	 * (network, console, file, imgui, etc.).
	 *
	 * For now this is sufficient, and time is better spent elsewhere.
	 * - Callan
	 */
	class Logger
	{
	public:
		/// Runtime log level filter (for toggling verbosity without recompiling)
		static void set_level(LogLevel level) { s_level = level; }
		static LogLevel level() { return s_level; }

		template <typename... Args>
		static void trace(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args)
		{
			log(LogLevel::Trace, "trace", "36", loc, fmt, std::forward<Args>(args)...);
		}

		template <typename... Args>
		static void info(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args)
		{
			log(LogLevel::Info, "info", "32", loc, fmt, std::forward<Args>(args)...);
		}

		template <typename... Args>
		static void warn(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args)
		{
			log(LogLevel::Warn, "warn", "33", loc, fmt, std::forward<Args>(args)...);
		}

		template <typename... Args>
		static void error(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args)
		{
			log(LogLevel::Error, "error", "31", loc, fmt, std::forward<Args>(args)...);
		}

	private:
		static double elapsed_seconds()
		{
			using clock							 = std::chrono::steady_clock;
			static const clock::time_point start = clock::now();
			return std::chrono::duration<double>(clock::now() - start).count();
		}

		template <typename... Args>
		static void
		log(LogLevel level,
			const char* label,
			const char* color,
			std::source_location loc,
			fmt::format_string<Args...> fmt,
			Args&&... args)
		{
			if (level < s_level)
				return;

			auto message = fmt::format(fmt, std::forward<Args>(args)...);

			std::scoped_lock lock(s_mutex);
			// Extract just the filename from the full path
			const char* file  = loc.file_name();
			const char* slash = file;
			for (const char* p = file; *p; ++p)
			{
				if (*p == '/' || *p == '\\')
					slash = p + 1;
			}

			// Columns: dim time | colored level | message | dim file:line
			fmt::print(
				stderr,
				"\033[38;5;240m{:>8.3f}\033[m  \033[{}m{:<5}\033[m  {}  \033[38;5;240m({}:{})\033[m\n",
				elapsed_seconds(),
				color,
				label,
				message,
				slash,
				loc.line());
		}

		static inline std::mutex s_mutex;
		static inline LogLevel s_level = LogLevel::Trace;
	};
}
