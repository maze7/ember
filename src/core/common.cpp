#include <cstdio>
#include <ember/core/common.h>

namespace ember
{
	bool assert_fail(const char* condition, const char* msg, const char* file, int line, const char* func)
	{
		std::fprintf(
			stderr,
			"[assert] %s:%d in %s: %s%s%s\n",
			file ? file : "<unknown>",
			line,
			func ? func : "<unknown>",
			condition ? condition : "<unknown>",
			msg ? " - " : "",
			msg ? msg : "");

		return true;
	}
}
