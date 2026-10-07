#include <ember/script/dmath.h>

#include <cmath>
#include <cstring>

/**
 * After fdlibm (Sun Microsystems, 1993; "Developed at SunPro"), whose algorithms every libm of note
 * descends from: argument reduction with split constants, then short polynomials. Written out in
 * adds and multiplies on doubles, with sqrt, which IEEE 754 rounds the same everywhere, so the
 * result is the same everywhere. std::ldexp and std::fmod are exact operations and stay.
 */
namespace ember::script::dmath
{
	namespace
	{
		[[nodiscard]] u32 high_word(f64 x) noexcept
		{
			u64 bits;
			std::memcpy(&bits, &x, sizeof bits);
			return static_cast<u32>(bits >> 32);
		}

		[[nodiscard]] u32 low_word(f64 x) noexcept
		{
			u64 bits;
			std::memcpy(&bits, &x, sizeof bits);
			return static_cast<u32>(bits);
		}

		[[nodiscard]] f64 with_high_word(f64 x, u32 high) noexcept
		{
			u64 bits;
			std::memcpy(&bits, &x, sizeof bits);
			bits = (static_cast<u64>(high) << 32) | (bits & 0xffffffffull);
			std::memcpy(&x, &bits, sizeof x);
			return x;
		}

		constexpr f64 HALF = 0.5, ONE = 1.0, TWO = 2.0;

		// Half pi, in pieces that multiply exactly by a small integer: the reduction keeps every bit.
		constexpr f64 INV_PIO2 = 6.36619772367581382433e-01;
		constexpr f64 PIO2_1   = 1.57079632673412561417e+00;
		constexpr f64 PIO2_1T  = 6.07710050650619224932e-11;
		constexpr f64 PIO2_2   = 6.07710050630396597660e-11;
		constexpr f64 PIO2_2T  = 2.02226624879595063154e-21;
		constexpr f64 PIO2_3   = 2.02226624871116645580e-21;
		constexpr f64 PIO2_3T  = 8.47842766036889956997e-32;
		constexpr f64 TWO_PI   = 6.28318530717958647693e+00;

		/** x = n * pi/2 + (y0 + y1), |y0 + y1| <= pi/4; the quadrant n, modulo four. */
		[[nodiscard]] int reduce_pio2(f64 x, f64& y0, f64& y1) noexcept
		{
			// An angle no gameplay ever means is brought down first: exactly, since fmod is.
			if (std::fabs(x) > 1.0e6)
				x = std::fmod(x, TWO_PI);

			const f64 t		= std::fabs(x);
			const f64 fn	= std::floor(t * INV_PIO2 + HALF);
			const int n		= static_cast<int>(fn);
			const u32 ix	= high_word(t) & 0x7fffffff;
			const u32 j		= ix >> 20;

			f64 r = t - fn * PIO2_1;
			f64 w = fn * PIO2_1T;
			y0	  = r - w;
			u32 i = j - ((high_word(y0) >> 20) & 0x7ff);
			if (i > 16)
			{
				const f64 t2 = r;
				w			 = fn * PIO2_2;
				r			 = t2 - w;
				w			 = fn * PIO2_2T - ((t2 - r) - w);
				y0			 = r - w;
				i			 = j - ((high_word(y0) >> 20) & 0x7ff);
				if (i > 49)
				{
					const f64 t3 = r;
					w			 = fn * PIO2_3;
					r			 = t3 - w;
					w			 = fn * PIO2_3T - ((t3 - r) - w);
					y0			 = r - w;
				}
			}
			y1 = (r - y0) - w;

			if (x < 0.0)
			{
				y0 = -y0;
				y1 = -y1;
				return -n;
			}
			return n;
		}

		constexpr f64 S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03, S3 = -1.98412698298579493134e-04,
					  S4 = 2.75573137070700676789e-06, S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;

		/** sin on [-pi/4, pi/4], x the leading part and y the tail; iy says whether there is a tail. */
		[[nodiscard]] f64 kernel_sin(f64 x, f64 y, bool tail) noexcept
		{
			const f64 z = x * x;
			const f64 v = z * x;
			const f64 r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
			if (!tail)
				return x + v * (S1 + z * r);
			return x - ((z * (HALF * y - v * r) - y) - v * S1);
		}

		constexpr f64 C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03, C3 = 2.48015872894767294178e-05,
					  C4 = -2.75573143513906633035e-07, C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

		[[nodiscard]] f64 kernel_cos(f64 x, f64 y) noexcept
		{
			const u32 ix = high_word(x) & 0x7fffffff;
			const f64 z	 = x * x;
			const f64 r	 = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
			if (ix < 0x3fd33333) // |x| < 0.3
				return ONE - (HALF * z - (z * r - x * y));

			// qx = |x| / 4, or 0.28125 past 0.78125: exact either way.
			const f64 qx = ix > 0x3fe90000 ? 0.28125 : std::fabs(x) * 0.25;
			const f64 hz = HALF * z - qx;
			const f64 a	 = ONE - qx;
			return a - (hz - (z * r - x * y));
		}

		constexpr f64 ATAN_HI[4] = {4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01,
									1.57079632679489655800e+00};
		constexpr f64 ATAN_LO[4] = {2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17,
									6.12323399573676603587e-17};
		constexpr f64 AT[11]	 = {3.33333333333329318027e-01,	-1.99999999998764832476e-01, 1.42857142725034663711e-01,
									-1.11111104054623557880e-01, 9.09088713343650656196e-02,	-7.69187620504482999495e-02,
									6.66107313738753120669e-02,	-5.83357013379057348645e-02, 4.97687799461593236017e-02,
									-3.65315727442169155270e-02, 1.62858201153657823623e-02};

		constexpr f64 LN2_HI = 6.93147180369123816490e-01, LN2_LO = 1.90821492927058770002e-10, INV_LN2 = 1.44269504088896338700e+00;
		constexpr f64 P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
					  P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08;
		constexpr f64 LG1 = 6.666666666666735130e-01, LG2 = 3.999999999940941908e-01, LG3 = 2.857142874366239149e-01,
					  LG4 = 2.222219843214978396e-01, LG5 = 1.818357216161805012e-01, LG6 = 1.531383769920937332e-01,
					  LG7 = 1.479819860511658591e-01;
	}

	f64 sin(f64 x) noexcept
	{
		const u32 ix = high_word(x) & 0x7fffffff;
		if (ix <= 0x3fe921fb) // |x| <= pi/4
			return kernel_sin(x, 0.0, false);
		if (ix >= 0x7ff00000) // inf or nan
			return x - x;

		f64 y0, y1;
		switch (reduce_pio2(x, y0, y1) & 3)
		{
			case 0:
				return kernel_sin(y0, y1, true);
			case 1:
				return kernel_cos(y0, y1);
			case 2:
				return -kernel_sin(y0, y1, true);
			default:
				return -kernel_cos(y0, y1);
		}
	}

	f64 cos(f64 x) noexcept
	{
		const u32 ix = high_word(x) & 0x7fffffff;
		if (ix <= 0x3fe921fb)
			return kernel_cos(x, 0.0);
		if (ix >= 0x7ff00000)
			return x - x;

		f64 y0, y1;
		switch (reduce_pio2(x, y0, y1) & 3)
		{
			case 0:
				return kernel_cos(y0, y1);
			case 1:
				return -kernel_sin(y0, y1, true);
			case 2:
				return -kernel_cos(y0, y1);
			default:
				return kernel_sin(y0, y1, true);
		}
	}

	f64 tan(f64 x) noexcept { return sin(x) / cos(x); }

	f64 atan(f64 x) noexcept
	{
		u32 hx		 = high_word(x);
		const u32 ix = hx & 0x7fffffff;
		if (ix >= 0x44100000) // |x| >= 2^66
		{
			if (ix > 0x7ff00000 || (ix == 0x7ff00000 && low_word(x) != 0))
				return x + x; // nan
			return hx > 0x7fffffff ? -(ATAN_HI[3] + ATAN_LO[3]) : ATAN_HI[3] + ATAN_LO[3];
		}

		int id;
		if (ix < 0x3fdc0000) // |x| < 0.4375
		{
			if (ix < 0x3e400000) // |x| < 2^-27
				return x;
			id = -1;
		}
		else
		{
			x = std::fabs(x);
			if (ix < 0x3ff30000) // |x| < 1.1875
			{
				if (ix < 0x3fe60000) // |x| < 0.6875
				{
					id = 0;
					x  = (TWO * x - ONE) / (TWO + x);
				}
				else
				{
					id = 1;
					x  = (x - ONE) / (x + ONE);
				}
			}
			else
			{
				if (ix < 0x40038000) // |x| < 2.4375
				{
					id = 2;
					x  = (x - 1.5) / (ONE + 1.5 * x);
				}
				else
				{
					id = 3;
					x  = -ONE / x;
				}
			}
		}

		const f64 z	 = x * x;
		const f64 w	 = z * z;
		const f64 s1 = z * (AT[0] + w * (AT[2] + w * (AT[4] + w * (AT[6] + w * (AT[8] + w * AT[10])))));
		const f64 s2 = w * (AT[1] + w * (AT[3] + w * (AT[5] + w * (AT[7] + w * AT[9]))));
		if (id < 0)
			return x - x * (s1 + s2);

		const f64 r = ATAN_HI[id] - ((x * (s1 + s2) - ATAN_LO[id]) - x);
		return hx > 0x7fffffff ? -r : r;
	}

	f64 atan2(f64 y, f64 x) noexcept
	{
		constexpr f64 PI	 = 3.1415926535897931160E+00;
		constexpr f64 PI_O_2 = 1.5707963267948965580E+00;
		constexpr f64 PI_O_4 = 7.8539816339744827900E-01;
		constexpr f64 PI_LO	 = 1.2246467991473531772E-16;

		const u32 hx = high_word(x), ix = hx & 0x7fffffff, lx = low_word(x);
		const u32 hy = high_word(y), iy = hy & 0x7fffffff, ly = low_word(y);
		if (((ix | ((lx | (0u - lx)) >> 31)) > 0x7ff00000) || ((iy | ((ly | (0u - ly)) >> 31)) > 0x7ff00000))
			return x + y; // nan
		if (((hx - 0x3ff00000) | lx) == 0)
			return atan(y); // x == 1

		const u32 m = ((hy >> 31) & 1) | ((hx >> 30) & 2); // the quadrant
		if ((iy | ly) == 0)							   // y == 0
		{
			switch (m)
			{
				case 0:
				case 1:
					return y;
				case 2:
					return PI + PI_LO;
				default:
					return -PI - PI_LO;
			}
		}
		if ((ix | lx) == 0) // x == 0
			return hy > 0x7fffffff ? -PI_O_2 - PI_LO : PI_O_2 + PI_LO;
		if (ix == 0x7ff00000) // x infinite
		{
			if (iy == 0x7ff00000)
			{
				switch (m)
				{
					case 0:
						return PI_O_4 + PI_LO;
					case 1:
						return -PI_O_4 - PI_LO;
					case 2:
						return 3.0 * PI_O_4 + PI_LO;
					default:
						return -3.0 * PI_O_4 - PI_LO;
				}
			}
			switch (m)
			{
				case 0:
					return 0.0;
				case 1:
					return -0.0;
				case 2:
					return PI + PI_LO;
				default:
					return -PI - PI_LO;
			}
		}
		if (iy == 0x7ff00000) // y infinite
			return hy > 0x7fffffff ? -PI_O_2 - PI_LO : PI_O_2 + PI_LO;

		// The exponents' difference: how far |y/x| is from one, in powers of two.
		const i32 k = (static_cast<i32>(iy) - static_cast<i32>(ix)) >> 20;
		f64 z;
		if (k > 60)
			z = PI_O_2 + HALF * PI_LO; // |y/x| > 2^60
		else if (hx > 0x7fffffff && k < -60)
			z = 0.0; // |y/x| < 2^-60, x negative
		else
			z = atan(std::fabs(y / x));

		switch (m)
		{
			case 0:
				return z;
			case 1:
				return -z;
			case 2:
				return PI - (z - PI_LO);
			default:
				return (z - PI_LO) - PI;
		}
	}

	f64 asin(f64 x) noexcept
	{
		if (std::fabs(x) > ONE)
			return (x - x) / (x - x);
		return atan2(x, std::sqrt((ONE - x) * (ONE + x)));
	}

	f64 acos(f64 x) noexcept
	{
		if (std::fabs(x) > ONE)
			return (x - x) / (x - x);
		return atan2(std::sqrt((ONE - x) * (ONE + x)), x);
	}

	f64 exp(f64 x) noexcept
	{
		constexpr f64 O_THRESHOLD = 7.09782712893383973096e+02;
		constexpr f64 U_THRESHOLD = -7.45133219101941108420e+02;
		constexpr f64 LN2HI[2]	  = {LN2_HI, -LN2_HI};
		constexpr f64 LN2LO[2]	  = {LN2_LO, -LN2_LO};
		constexpr f64 HALVES[2]	  = {0.5, -0.5};

		u32 hx		  = high_word(x);
		const u32 xsb = (hx >> 31) & 1;
		hx &= 0x7fffffff;

		if (hx >= 0x40862e42) // |x| >= 709.78
		{
			if (hx >= 0x7ff00000)
			{
				if (((hx & 0xfffff) | low_word(x)) != 0)
					return x + x; // nan
				return xsb == 0 ? x : 0.0;
			}
			if (x > O_THRESHOLD)
				return HUGE_VAL;
			if (x < U_THRESHOLD)
				return 0.0;
		}

		f64 hi = 0.0, lo = 0.0;
		int k = 0;
		if (hx > 0x3fd62e42) // |x| > 0.5 ln2
		{
			if (hx < 0x3ff0a2b2) // |x| < 1.5 ln2
			{
				hi = x - LN2HI[xsb];
				lo = LN2LO[xsb];
				k  = 1 - static_cast<int>(xsb) - static_cast<int>(xsb);
			}
			else
			{
				k			= static_cast<int>(INV_LN2 * x + HALVES[xsb]);
				const f64 t = static_cast<f64>(k);
				hi			= x - t * LN2HI[0];
				lo			= t * LN2LO[0];
			}
			x = hi - lo;
		}
		else if (hx < 0x3e300000) // |x| < 2^-28
		{
			return ONE + x;
		}

		const f64 t = x * x;
		const f64 c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
		if (k == 0)
			return ONE - ((x * c) / (c - TWO) - x);

		const f64 y = ONE - ((lo - (x * c) / (TWO - c)) - hi);
		return std::ldexp(y, k);
	}

	f64 log(f64 x) noexcept
	{
		constexpr f64 TWO54 = 1.80143985094819840000e+16;

		u32 hx		 = high_word(x);
		const u32 lx = low_word(x);
		int k		 = 0;

		if (static_cast<i32>(hx) < 0x00100000) // x < 2^-1022: zero, negative or subnormal
		{
			if (((hx & 0x7fffffff) | lx) == 0)
				return -HUGE_VAL;
			if (static_cast<i32>(hx) < 0)
				return (x - x) / 0.0; // nan
			k -= 54;
			x *= TWO54;
			hx = high_word(x);
		}
		if (hx >= 0x7ff00000)
			return x + x;

		k += static_cast<int>(hx >> 20) - 1023;
		hx &= 0x000fffff;
		const u32 i = (hx + 0x95f64) & 0x100000;
		x			= with_high_word(x, hx | (i ^ 0x3ff00000)); // x in [sqrt(2)/2, sqrt(2))
		k += static_cast<int>(i >> 20);
		const f64 f	 = x - ONE;
		const f64 dk = static_cast<f64>(k);

		if ((0x000fffff & (2 + hx)) < 3) // f tiny
		{
			if (f == 0.0)
				return k == 0 ? 0.0 : dk * LN2_HI + dk * LN2_LO;
			const f64 r = f * f * (HALF - 0.33333333333333333 * f);
			return k == 0 ? f - r : dk * LN2_HI - ((r - dk * LN2_LO) - f);
		}

		const f64 s	 = f / (TWO + f);
		const f64 z	 = s * s;
		const f64 w	 = z * z;
		const f64 t1 = w * (LG2 + w * (LG4 + w * LG6));
		const f64 t2 = z * (LG1 + w * (LG3 + w * (LG5 + w * LG7)));
		const f64 r	 = t2 + t1;
		const i32 ii = static_cast<i32>(hx) - 0x6147a;
		const i32 j	 = 0x6b851 - static_cast<i32>(hx);
		if ((ii | j) > 0)
		{
			const f64 hfsq = HALF * f * f;
			return k == 0 ? f - (hfsq - s * (hfsq + r)) : dk * LN2_HI - ((hfsq - (s * (hfsq + r) + dk * LN2_LO)) - f);
		}
		return k == 0 ? f - s * (f - r) : dk * LN2_HI - ((s * (f - r) - dk * LN2_LO) - f);
	}

	f64 pow(f64 x, f64 y) noexcept
	{
		if (y == 0.0 || x == ONE)
			return ONE;
		if (std::isnan(x) || std::isnan(y))
			return x + y;
		if (y == TWO)
			return x * x;
		if (y == ONE)
			return x;
		if (y == -ONE)
			return ONE / x;
		if (y == HALF && x >= 0.0)
			return std::sqrt(x);
		if (y == 3.0)
			return x * x * x;
		if (x == 0.0)
			return y > 0.0 ? 0.0 : HUGE_VAL;

		// A negative base takes an integer power only, with the sign of the odd ones.
		if (x < 0.0)
		{
			if (std::floor(y) != y)
				return (x - x) / (x - x);
			const f64 magnitude = exp(y * log(-x));
			const bool odd		= std::fmod(std::fabs(y), TWO) == ONE;
			return odd ? -magnitude : magnitude;
		}

		return exp(y * log(x));
	}
}
