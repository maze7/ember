#include <ember/net/time_dilation.h>

#include <gtest/gtest.h>

namespace
{
	using namespace ember;
	using namespace ember::net;

	// Default aim with a deviation of 0.5 ticks: 0.5 + 3 * 0.5 = 2 ticks early.
	constexpr f32 DEVIATION = 0.5f;
	constexpr f32 AIM		= 2.0f;

	[[nodiscard]] CommandTiming timing(f32 mean, u8 epoch = 0)
	{
		return {.epoch = epoch, .mean = mean, .deviation = DEVIATION};
	}

	TEST(TimeDilation, RunsInRealTimeUntilTold)
	{
		TimeDilation dilation;
		EXPECT_EQ(dilation.time_scale(), 1.0f);
		EXPECT_EQ(dilation.take_jump(), 0);
		EXPECT_EQ(dilation.epoch(), 0);
	}

	TEST(TimeDilation, SmallErrorsInsideTheDeadbandChangeNothing)
	{
		TimeDilation dilation;
		dilation.report(timing(AIM + 0.4f));
		EXPECT_EQ(dilation.time_scale(), 1.0f);
		EXPECT_NEAR(dilation.aim(), AIM, 1e-6f);
		dilation.report(timing(AIM - 0.4f));
		EXPECT_EQ(dilation.time_scale(), 1.0f);
	}

	TEST(TimeDilation, LateCommandsSpeedTheClockUpAndEarlyOnesSlowItDown)
	{
		TimeDilation dilation;

		dilation.report(timing(AIM - 1.5f)); // a tick past the deadband, late
		EXPECT_NEAR(dilation.time_scale(), 1.02f, 1e-6f);

		dilation.report(timing(AIM + 2.5f)); // two ticks past it, early
		EXPECT_NEAR(dilation.time_scale(), 0.96f, 1e-6f);

		dilation.report(timing(AIM + 20.0f)); // far early, but not enough to jump: clamped
		EXPECT_NEAR(dilation.time_scale(), 0.95f, 1e-6f);
	}

	TEST(TimeDilation, FallingFarBehindJumpsForwardOnceAndStartsAnEpoch)
	{
		TimeDilation dilation;

		dilation.report(timing(AIM - 10.0f));
		EXPECT_EQ(dilation.take_jump(), 0) << "one report is not enough evidence";

		dilation.report(timing(AIM - 10.0f));
		EXPECT_EQ(dilation.take_jump(), 10);
		EXPECT_EQ(dilation.take_jump(), 0) << "taken once";
		EXPECT_EQ(dilation.epoch(), 1);
		EXPECT_EQ(dilation.time_scale(), 1.0f);

		// Reports still in flight describe the old clock and are ignored.
		dilation.report(timing(AIM - 10.0f, 0));
		dilation.report(timing(AIM - 10.0f, 0));
		EXPECT_EQ(dilation.take_jump(), 0);
		EXPECT_EQ(dilation.time_scale(), 1.0f);

		dilation.report(timing(AIM - 1.5f, 1));
		EXPECT_NEAR(dilation.time_scale(), 1.02f, 1e-6f);
	}

	TEST(TimeDilation, RunningFarAheadJumpsBack)
	{
		TimeDilation dilation;
		dilation.report(timing(AIM + 40.0f));
		dilation.report(timing(AIM + 40.0f));
		EXPECT_EQ(dilation.take_jump(), -40);
		EXPECT_EQ(dilation.epoch(), 1);
	}

	TEST(TimeDilation, AJitteryLinkIsAimedFurtherAhead)
	{
		TimeDilation dilation;
		dilation.report({.epoch = 0, .mean = 2.0f, .deviation = 1.5f}); // aim 0.5 + 4.5 = 5
		EXPECT_NEAR(dilation.aim(), 5.0f, 1e-6f);
		EXPECT_GT(dilation.time_scale(), 1.0f) << "2 ticks early is not enough on this link";
	}

	TEST(TimeDilation, AJoiningClientStartsARoundTripAndTheHeadroomAhead)
	{
		// 100 ms at 60 Hz is 6 ticks, plus the 2 ticks of headroom before anything is measured.
		EXPECT_EQ(initial_client_tick(1000, 0.1, 1.0 / 60.0), 1008u);
		EXPECT_EQ(initial_client_tick(1000, 0.0, 1.0 / 60.0), 1002u);
	}
}
