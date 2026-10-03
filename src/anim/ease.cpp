#include <ember/anim/rig.h>

#include <cmath>
#include <numbers>

namespace ember::anim
{
	f32 ease(Ease ease, f32 t) noexcept
	{
		// game2's formulas, so what was tuned there feels the same here.
		constexpr f32 PI   = std::numbers::pi_v<f32>;
		constexpr f32 BACK = 1.70158f;

		switch (ease)
		{
			case Ease::Step:
				return t >= 1.0f ? 1.0f : 0.0f;
			case Ease::Linear:
				return t;
			case Ease::QuadIn:
				return t * t;
			case Ease::QuadOut:
				return t * (2.0f - t);
			case Ease::QuadInOut:
				return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
			case Ease::CubicIn:
				return t * t * t;
			case Ease::CubicOut:
			{
				const f32 u = t - 1.0f;
				return u * u * u + 1.0f;
			}
			case Ease::CubicInOut:
				return t < 0.5f ? 4.0f * t * t * t : 1.0f + (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f);
			case Ease::QuartIn:
				return t * t * t * t;
			case Ease::QuartOut:
			{
				const f32 u = t - 1.0f;
				return 1.0f - u * u * u * u;
			}
			case Ease::QuartInOut:
			{
				const f32 u = t - 1.0f;
				return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - 8.0f * u * u * u * u;
			}
			case Ease::SineIn:
				return 1.0f - std::cos(t * PI * 0.5f);
			case Ease::SineOut:
				return std::sin(t * PI * 0.5f);
			case Ease::SineInOut:
				return 0.5f * (1.0f - std::cos(t * PI));
			case Ease::ExpoIn:
				return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f));
			case Ease::ExpoOut:
				return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
			case Ease::ExpoInOut:
				if (t == 0.0f || t == 1.0f)
					return t;
				return t < 0.5f ? 0.5f * std::pow(2.0f, 20.0f * t - 10.0f)
								: 1.0f - 0.5f * std::pow(2.0f, -20.0f * t + 10.0f);
			case Ease::BackIn:
				return t * t * ((BACK + 1.0f) * t - BACK);
			case Ease::BackOut:
			{
				const f32 u = t - 1.0f;
				return u * u * ((BACK + 1.0f) * u + BACK) + 1.0f;
			}
			case Ease::BackInOut:
			{
				constexpr f32 c = BACK * 1.525f;
				const f32 s		= t * 2.0f;
				if (s < 1.0f)
					return 0.5f * s * s * ((c + 1.0f) * s - c);
				const f32 r = s - 2.0f;
				return 0.5f * (r * r * ((c + 1.0f) * r + c) + 2.0f);
			}
			case Ease::ElasticOut:
				if (t == 0.0f || t == 1.0f)
					return t;
				return std::pow(2.0f, -10.0f * t) * std::sin((t - 0.075f) * (2.0f * PI) / 0.3f) + 1.0f;
			case Ease::BounceOut:
			{
				constexpr f32 n = 7.5625f;
				constexpr f32 d = 2.75f;
				if (t < 1.0f / d)
					return n * t * t;
				if (t < 2.0f / d)
					return n * (t - 1.5f / d) * (t - 1.5f / d) + 0.75f;
				if (t < 2.5f / d)
					return n * (t - 2.25f / d) * (t - 2.25f / d) + 0.9375f;
				return n * (t - 2.625f / d) * (t - 2.625f / d) + 0.984375f;
			}
			case Ease::Count:
				break;
		}
		return t;
	}
}
