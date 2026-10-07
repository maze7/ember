#include <ember/script/dmath.h>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace
{
	using namespace ember;

	/** Units in the last place between two doubles of the same sign. */
	[[nodiscard]] f64 ulps(f64 a, f64 b) noexcept
	{
		if (a == b)
			return 0.0;
		const f64 scale = std::ldexp(1.0, std::ilogb(std::fabs(a)) - 52);
		return std::fabs(a - b) / scale;
	}

	/** A deterministic sequence of arguments over a range: the same on every run. */
	struct Sampler
	{
		u64 state = 0x9e3779b97f4a7c15ull;

		[[nodiscard]] f64 next(f64 low, f64 high) noexcept
		{
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			const f64 unit = static_cast<f64>(state >> 11) / 9007199254740992.0;
			return low + (high - low) * unit;
		}
	};

	TEST(Dmath, TrigonometryAgreesWithLibmToAFewUlps)
	{
		Sampler sampler;
		for (int i = 0; i < 20000; ++i)
		{
			const f64 x = sampler.next(-40.0, 40.0);
			EXPECT_LE(ulps(script::dmath::sin(x), std::sin(x)), 2.0) << "sin " << x;
			EXPECT_LE(ulps(script::dmath::cos(x), std::cos(x)), 2.0) << "cos " << x;
			EXPECT_NEAR(script::dmath::tan(x), std::tan(x), std::fabs(std::tan(x)) * 1e-14 + 1e-15) << "tan " << x;
		}
	}

	TEST(Dmath, InverseTrigonometryAgreesWithLibm)
	{
		Sampler sampler;
		for (int i = 0; i < 20000; ++i)
		{
			const f64 x = sampler.next(-1.0, 1.0);
			EXPECT_LE(ulps(script::dmath::asin(x), std::asin(x)), 4.0) << "asin " << x;
			EXPECT_LE(ulps(script::dmath::acos(x), std::acos(x)), 4.0) << "acos " << x;

			const f64 y = sampler.next(-1000.0, 1000.0);
			EXPECT_LE(ulps(script::dmath::atan(y), std::atan(y)), 2.0) << "atan " << y;
			const f64 z = sampler.next(-1000.0, 1000.0);
			EXPECT_LE(ulps(script::dmath::atan2(y, z), std::atan2(y, z)), 4.0) << "atan2 " << y << ", " << z;
		}
	}

	TEST(Dmath, Atan2Quadrants)
	{
		constexpr f64 PI = 3.14159265358979323846;
		EXPECT_DOUBLE_EQ(script::dmath::atan2(0.0, 1.0), 0.0);
		EXPECT_DOUBLE_EQ(script::dmath::atan2(1.0, 0.0), PI / 2.0);
		EXPECT_DOUBLE_EQ(script::dmath::atan2(0.0, -1.0), PI);
		EXPECT_DOUBLE_EQ(script::dmath::atan2(-1.0, 0.0), -PI / 2.0);
		EXPECT_DOUBLE_EQ(script::dmath::atan2(1.0, 1.0), PI / 4.0);
		EXPECT_DOUBLE_EQ(script::dmath::atan2(-1.0, -1.0), -3.0 * PI / 4.0);
		EXPECT_TRUE(std::isnan(script::dmath::atan2(std::nan(""), 1.0)));
	}

	TEST(Dmath, ExponentialsAgreeWithLibm)
	{
		Sampler sampler;
		for (int i = 0; i < 20000; ++i)
		{
			const f64 x = sampler.next(-700.0, 700.0);
			EXPECT_LE(ulps(script::dmath::exp(x), std::exp(x)), 2.0) << "exp " << x;

			const f64 y = sampler.next(1e-300, 1e300);
			EXPECT_LE(ulps(script::dmath::log(y), std::log(y)), 2.0) << "log " << y;

			const f64 base = sampler.next(0.01, 50.0);
			const f64 power = sampler.next(-8.0, 8.0);
			EXPECT_NEAR(script::dmath::pow(base, power), std::pow(base, power), std::pow(base, power) * 1e-13)
				<< "pow " << base << ", " << power;
		}
	}

	TEST(Dmath, EdgesMatchTheStandard)
	{
		EXPECT_EQ(script::dmath::exp(1000.0), std::numeric_limits<f64>::infinity());
		EXPECT_EQ(script::dmath::exp(-1000.0), 0.0);
		EXPECT_EQ(script::dmath::log(0.0), -std::numeric_limits<f64>::infinity());
		EXPECT_TRUE(std::isnan(script::dmath::log(-1.0)));
		EXPECT_EQ(script::dmath::log(1.0), 0.0);
		EXPECT_EQ(script::dmath::pow(2.0, 10.0), 1024.0);
		EXPECT_EQ(script::dmath::pow(-2.0, 3.0), -8.0);
		EXPECT_EQ(script::dmath::pow(-2.0, 2.0), 4.0);
		EXPECT_TRUE(std::isnan(script::dmath::pow(-2.0, 0.5)));
		EXPECT_EQ(script::dmath::pow(0.0, 0.0), 1.0);
		EXPECT_EQ(script::dmath::pow(9.0, 0.5), 3.0);
		EXPECT_EQ(script::dmath::sin(0.0), 0.0);
		EXPECT_EQ(script::dmath::cos(0.0), 1.0);
		EXPECT_TRUE(std::isnan(script::dmath::sin(std::numeric_limits<f64>::infinity())));
		EXPECT_TRUE(std::isnan(script::dmath::asin(2.0)));
	}

	TEST(Dmath, LargeArgumentsReduceWithoutDrift)
	{
		// A million radians: the reduction keeps every bit it can, and the answer stays close to libm's.
		for (const f64 x : {1.0e5, 3.3e5, 9.99e5, -7.7e5})
		{
			EXPECT_NEAR(script::dmath::sin(x), std::sin(x), 1e-9) << x;
			EXPECT_NEAR(script::dmath::cos(x), std::cos(x), 1e-9) << x;
		}
	}
}
