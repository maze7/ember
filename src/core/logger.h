#pragma once

#include "fmt/format.h"
#include <iostream>
#include <mutex>

#define EMBER_TRACE(...) Ember::Log::trace(__VA_ARGS__)
#define EMBER_INFO(...)  Ember::Log::info(__VA_ARGS__)
#define EMBER_WARN(...)  Ember::Log::warn(__VA_ARGS__)
#define EMBER_ERROR(...) Ember::Log::error(__VA_ARGS__)

namespace Ember
{
	class Log
	{
	public:
		// Log a trace-level message
		template <typename... Args>
		static void trace(fmt::format_string<Args...> fmt, Args&&... args) {
			log("log", "36", fmt, std::forward<Args>(args)...);
		}

		// Log an info-level message
		template <typename... Args>
		static void info(fmt::format_string<Args...> fmt, Args&&... args) {
			log("info", "32", fmt, std::forward<Args>(args)...);
		}

		// Log a warning-level message
		template <typename... Args>
		static void warn(fmt::format_string<Args...> fmt, Args&&... args) {
			log("warn", "33", fmt, std::forward<Args>(args)...);
		}

		// Log an error-level message
		template <typename... Args>
		static void error(fmt::format_string<Args...> fmt, Args&&... args) {
			log("error", "31", fmt, std::forward<Args>(args)...);
		}

	private:
		// ASCII Encodes a string to be a certain color, utility function used by above log functions
		static constexpr std::string colored(const char* str, const char* color) {
			return std::string("\033[") + color + "m" + str + "\033[m";
		}

		// Generic log builder, handles label, color and formatting of a single log line.
		template <typename... Args>
		static void log(const char* label, const char* color, fmt::format_string<Args...> fmt, Args&&... args) {
			// lock or block until mutex is available
			std::scoped_lock lock(s_mutex);
			std::cout << "[" << colored(label, color) << "] " << fmt::format(fmt, std::forward<Args>(args)...)
				<< std::endl;
		}

		// static mutex is used to ensure multiple loggers or threads do not write at the same time
		static std::mutex s_mutex;
	};
}
