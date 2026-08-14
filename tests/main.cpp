#include <ember/memory/memory.h>
#include <gtest/gtest.h>
#include <cstdio>

using namespace ember;

int main(int argc, char** argv)
{
	// Death tests fork (or re-exec) the binary; "threadsafe" keeps them correct
	// even with rpmalloc/tracker threads alive in the parent. Set before Init so
	// the command line can still override it.
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	::testing::InitGoogleTest(&argc, argv);

	MemorySystem memory_system;

	if (!memory_system)
	{
		std::fprintf(stderr, "memory::initialize() failed\n");
		return 1;
	}

	return RUN_ALL_TESTS();
} // leak reporting and shutdown happen here
