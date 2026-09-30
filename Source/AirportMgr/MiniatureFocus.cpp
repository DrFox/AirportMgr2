#include "MiniatureFocus.h"

namespace
{
	/** UE world units are centimetres; the lens formulas work in mm, as the renderer's do. */
	constexpr double MmPerUu = 10.0;

	/** tan of half the horizontal FOV, or zero when the FOV cannot describe a lens. */
	double HalfFovTan(double HorizontalFovDegrees)
	{
		if (HorizontalFovDegrees <= 0.0 || HorizontalFovDegrees >= 180.0)
		{
			return 0.0;
		}
		return FMath::Tan(FMath::DegreesToRadians(HorizontalFovDegrees * 0.5));
	}
}

double FMiniatureFocus::BlurAt(double FocusDistanceUu) const
{
	// A fade range that is empty or inverted is a step at FullBlurDistanceUu, not a division
	// by zero.
	if (FullBlurDistanceUu <= NoBlurDistanceUu)
	{
		return FocusDistanceUu >= FullBlurDistanceUu ? BlurAtInfinity : 0.0;
	}
	const double Alpha = FMath::Clamp(
		(FocusDistanceUu - NoBlurDistanceUu) / (FullBlurDistanceUu - NoBlurDistanceUu), 0.0, 1.0);
	return BlurAtInfinity * FMath::SmoothStep(0.0, 1.0, Alpha);
}

double FMiniatureFocus::SensorWidthMm(double FocusDistanceUu, double HorizontalFovDegrees) const
{
	const double T = HalfFovTan(HorizontalFovDegrees);
	const double S = FocusDistanceUu * MmPerUu;
	const double Blur = BlurAt(FocusDistanceUu);
	if (T <= 0.0 || S <= 0.0 || FStop <= 0.0 || Blur <= 0.0)
	{
		return 0.0;
	}

	// Solved exactly rather than with the s >> f shortcut, because at the 6 m close zoom the
	// focal length this produces is no longer negligible against s:
	//   B = f^2 / (N (s - f) w),  w = 2 T f
	//   => f = 2 B N T (s - f)  => f = 2BNT s / (1 + 2BNT)
	const double K = 2.0 * Blur * FStop * T;
	const double FocalLength = K * S / (1.0 + K);
	return 2.0 * T * FocalLength;
}

double FMiniatureFocus::BlurFraction(double SensorWidth, double FocusDistanceUu, double DistanceUu,
	double HorizontalFovDegrees) const
{
	const double T = HalfFovTan(HorizontalFovDegrees);
	const double S = FocusDistanceUu * MmPerUu;
	const double D = DistanceUu * MmPerUu;
	if (T <= 0.0 || S <= 0.0 || D <= 0.0 || SensorWidth <= 0.0 || FStop <= 0.0)
	{
		return 0.0;
	}

	const double FocalLength = 0.5 * SensorWidth / T;
	if (S <= FocalLength)
	{
		return 0.0;
	}

	// Infinity blur times the thin-lens depth term |d - s| / d.
	const double AtInfinity = FMath::Square(FocalLength) / (FStop * (S - FocalLength)) / SensorWidth;
	return AtInfinity * FMath::Abs(D - S) / D;
}
