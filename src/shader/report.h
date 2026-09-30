#pragma once

#include <ember/core/common.h>

#include <fmt/format.h>
#include <slang.h>

#include <iterator>

namespace ember::shader
{
	/**
	 * Appends to the caller's diagnostics in the one-line shape compilers use, "path:line: error:
	 * message", which terminals and editors turn into links. Slang's own diagnostics are forwarded
	 * as it wrote them. Every check reports and carries on, so one compile lists every problem.
	 */
	class Report
	{
	public:
		explicit Report(String& out) noexcept : m_out(out) {}

		template <class... Args>
		void error(StringView path, fmt::format_string<Args...> format, Args&&... args) noexcept
		{
			fmt::format_to(std::back_inserter(m_out), "{}: error: ", path);
			finish(format, std::forward<Args>(args)...);
		}

		template <class... Args>
		void error_at(StringView path, u32 line, fmt::format_string<Args...> format, Args&&... args) noexcept
		{
			fmt::format_to(std::back_inserter(m_out), "{}:{}: error: ", path, line);
			finish(format, std::forward<Args>(args)...);
		}

		void forward(slang::IBlob* diagnostics) noexcept
		{
			if (diagnostics == nullptr)
				return;

			// Slang's blob may or may not count its terminator; the text ends at the first null.
			const StringView text(static_cast<const char*>(diagnostics->getBufferPointer()),
								  diagnostics->getBufferSize());
			m_out.append(text.substr(0, text.find('\0')));
		}

		[[nodiscard]] bool failed() const noexcept { return m_failed; }

	private:
		template <class... Args> void finish(fmt::format_string<Args...> format, Args&&... args) noexcept
		{
			fmt::format_to(std::back_inserter(m_out), format, std::forward<Args>(args)...);
			m_out += '\n';
			m_failed = true;
		}

		String& m_out;
		bool m_failed = false;
	};
}
