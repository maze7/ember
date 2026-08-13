#include <ember/memory/memory.h>

#include <gtest/gtest.h>

#include <cstdio>

int main(int argc, char** argv)
{
	// Death tests fork (or re-exec) the binary; "threadsafe" keeps them correct
	// even with rpmalloc/tracker threads alive in the parent. Set before Init so
	// the command line can still override it.
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	::testing::InitGoogleTest(&argc, argv);

	if (!ember::memory::initialize())
	{
		std::fprintf(stderr, "memory::initialize() failed\n");
		return 1;
	}

	const int result = RUN_ALL_TESTS();

	// Doubles as the suite's leak gate: shutdown() reports live tracked blocks
	// and asserts the count is zero, so a leaky test fails loudly at exit.
	ember::memory::shutdown();
	return result;
}
