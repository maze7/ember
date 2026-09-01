#include <ember/app/main.h>

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
	#define NOMINMAX
#endif

#include <windows.h>

#include <shellapi.h>

// GUI subsystem entry; ember_add_game sets WIN32_EXECUTABLE. Tools keep
// writing their own console main.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int argc	 = 0;
	LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);

	if (wide == nullptr)
		return ember_main({});

	std::vector<std::string> utf8(static_cast<size_t>(argc));
	std::vector<const char*> argv(static_cast<size_t>(argc));

	for (int i = 0; i < argc; ++i)
	{
		const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
		utf8[i].resize(size > 0 ? static_cast<size_t>(size - 1) : 0);
		WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8[i].data(), size, nullptr, nullptr);
		argv[i] = utf8[i].c_str();
	}

	LocalFree(wide);

	return ember_main({.args = {argv.data(), argv.size()}});
}
