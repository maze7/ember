#include <jobs/fiber.h>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#if !defined(EMBER_PLATFORM_WINDOWS)
	#include <sys/wait.h>
#endif

using namespace ember;
using namespace ember::jobs;

namespace
{
	// host is the adopted test thread, a and b are created fibers.
	struct Fibers
	{
		Fiber* host = nullptr;
		Fiber* a	= nullptr;
		Fiber* b	= nullptr;

		u64 count	   = 0;
		u64 step	   = 0;
		bool ok		   = false;
		char trace[16] = {};
		u32 length	   = 0;

		void record(char c)
		{
			if (length + 1 < sizeof(trace))
				trace[length++] = c;
		}
	};

	void count_entry(void* arg)
	{
		auto& f = *static_cast<Fibers*>(arg);

		for (;;)
		{
			++f.count;
			fiber_switch(f.a, f.host);
		}
	}

	void chain_a_entry(void* arg)
	{
		auto& f = *static_cast<Fibers*>(arg);

		for (;;)
		{
			f.record('a');
			fiber_switch(f.a, f.b);
		}
	}

	void chain_b_entry(void* arg)
	{
		auto& f = *static_cast<Fibers*>(arg);

		for (;;)
		{
			f.record('b');
			fiber_switch(f.b, f.host);
		}
	}

	// More values live across each switch than there are callee saved registers, so the
	// registers and the spill slots on the fiber stack both have to survive the round trip.
	void registers_entry(void* arg)
	{
		auto& f = *static_cast<Fibers*>(arg);

		u64 v0 = 1, v1 = 2, v2 = 3, v3 = 4, v4 = 5, v5 = 6;
		f64 d0 = 0.5, d1 = 0.25;

		for (u32 i = 0; i < 1000; ++i)
		{
			fiber_switch(f.a, f.host);

			const u64 step	= f.step;
			v0			   += step;
			v1			   += step + 1;
			v2			   += step + 2;
			v3			   += step + 3;
			v4			   += step + 4;
			v5			   += step + 5;
			d0			   += f64(step);
			d1			   += 0.5;
		}

		const u64 steps = 999 * 1000 / 2;
		f.ok = v0 == 1 + steps && v1 == 2 + steps + 1000 && v2 == 3 + steps + 2000 && v3 == 4 + steps + 3000 &&
			   v4 == 5 + steps + 4000 && v5 == 6 + steps + 5000 && d0 == 0.5 + f64(steps) && d1 == 0.25 + 500.0;

		for (;;)
			fiber_switch(f.a, f.host);
	}

	// A frame larger than the 64 KiB stack runs off its bottom. Exiting normally afterwards
	// turns a silent overrun into a test failure.
	void overflow_entry(void*)
	{
		[[maybe_unused]] volatile u8 frame[66_kb];
		frame[0] = 1;
		std::_Exit(0);
	}

	// Linux reports the guard page hit as SIGSEGV and macOS as SIGBUS. Windows hands gtest
	// the exception code instead: access violation or stack overflow.
	bool died_of_stack_fault(int status)
	{
#if defined(EMBER_PLATFORM_WINDOWS)
		const u32 code = static_cast<u32>(status);
		return code == 0xC0000005u || code == 0xC00000FDu;
#else
		return WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
#endif
	}
}

TEST(Fiber, PingPong)
{
	Fibers f;
	f.host = fiber_adopt_thread();
	f.a	   = fiber_create({.entry = count_entry, .arg = &f});
	ASSERT_NE(f.a, nullptr);

	for (u64 i = 1; i <= 1000; ++i)
	{
		fiber_switch(f.host, f.a);
		ASSERT_EQ(f.count, i);
	}

	fiber_destroy(f.a);
	fiber_release_thread(f.host);
}

TEST(Fiber, ChainResumesWhereEachLeftOff)
{
	Fibers f;
	f.host = fiber_adopt_thread();
	f.a	   = fiber_create({.entry = chain_a_entry, .arg = &f});
	f.b	   = fiber_create({.entry = chain_b_entry, .arg = &f});
	ASSERT_NE(f.a, nullptr);
	ASSERT_NE(f.b, nullptr);

	fiber_switch(f.host, f.a);
	EXPECT_STREQ(f.trace, "ab");

	fiber_switch(f.host, f.a);
	EXPECT_STREQ(f.trace, "abab");

	fiber_destroy(f.b);
	fiber_destroy(f.a);
	fiber_release_thread(f.host);
}

TEST(Fiber, LiveValuesSurviveSwitches)
{
	Fibers f;
	f.host = fiber_adopt_thread();
	f.a	   = fiber_create({.entry = registers_entry, .arg = &f});
	ASSERT_NE(f.a, nullptr);

	fiber_switch(f.host, f.a);

	for (u64 step = 0; step < 1000; ++step)
	{
		f.step = step;
		fiber_switch(f.host, f.a);
	}

	EXPECT_TRUE(f.ok);

	fiber_destroy(f.a);
	fiber_release_thread(f.host);
}

TEST(Fiber, RoundTripCost)
{
	Fibers f;
	f.host = fiber_adopt_thread();
	f.a	   = fiber_create({.entry = count_entry, .arg = &f});
	ASSERT_NE(f.a, nullptr);

	constexpr u64 round_trips = 1'000'000;

	const auto start = std::chrono::steady_clock::now();

	for (u64 i = 0; i < round_trips; ++i)
		fiber_switch(f.host, f.a);

	const auto elapsed = std::chrono::steady_clock::now() - start;
	const f64 ns	   = std::chrono::duration<f64, std::nano>(elapsed).count() / f64(round_trips);

	EXPECT_EQ(f.count, round_trips);
	std::printf("[          ] %.1f ns per round trip (two switches)\n", ns);

	fiber_destroy(f.a);
	fiber_release_thread(f.host);
}

// The second stack is mapped directly below the first, so without a guard page the
// overrun would land in it and the process would exit normally.
TEST(FiberDeathTest, OverflowHitsGuardPage)
{
	EXPECT_EXIT(
		{
			Fiber* host		 = fiber_adopt_thread();
			Fiber* fiber	 = fiber_create({.stack_size = 64_kb, .entry = overflow_entry});
			Fiber* neighbour = fiber_create({.stack_size = 64_kb, .entry = overflow_entry});
			(void)neighbour;
			fiber_switch(host, fiber);
		},
		died_of_stack_fault,
		"");
}
