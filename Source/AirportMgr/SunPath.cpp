#include "SunPath.h"

FSunLighting FSunPath::At(double DayFraction) const
{
	// Wrap rather than clamp: a clock reading is unbounded and a caller reducing it first
	// is a step that can be forgotten. FMath::Fmod keeps the sign of its argument, so a
	// negative fraction needs the extra turn.
	double Fraction = FMath::Fmod(DayFraction, 1.0);
	if (Fraction < 0.0)
	{
		Fraction += 1.0;
	}

	// T is -1 at 06:00, 0 at noon, +1 at 18:00. Cosine of a quarter turn gives 1 at noon
	// falling to 0 at each end, so the elevation reaches MaxElevationDegrees exactly once
	// and MEETS the floor smoothly rather than stepping onto it - a step would read as the
	// sun snapping at dawn and dusk, twice every game day.
	constexpr double Noon = 0.5;
	constexpr double QuarterDay = 0.25;
	const double T = (Fraction - Noon) / QuarterDay;

	double Elevation = MinElevationDegrees;
	if (FMath::Abs(T) < 1.0)
	{
		const double Shape = FMath::Cos(T * UE_DOUBLE_HALF_PI);
		Elevation = FMath::Lerp(MinElevationDegrees, MaxElevationDegrees, Shape);
	}

	// Alpha is 0 at the floor and 1 at noon, and drives colour and brightness TOGETHER so
	// the two can never disagree about what time it is. Guarded because a path configured
	// with Min == Max is a legitimate "fixed sun" and must not divide by zero.
	const double Span = MaxElevationDegrees - MinElevationDegrees;
	const double Alpha = FMath::IsNearlyZero(Span)
		? 1.0
		: FMath::Clamp((Elevation - MinElevationDegrees) / Span, 0.0, 1.0);

	// A FULL 360 over the day, deliberately not reversed at dusk. The sun carrying on to
	// the north through the night is what makes the azimuth continuous across midnight;
	// turning it back would put a visible kink in the shadow direction twice a day.
	const double Azimuth = NoonAzimuthDegrees + 360.0 * (Fraction - Noon);

	FSunLighting Out;
	// Negative pitch points the light DOWN: elevation above the horizon, not below it.
	Out.Rotation = FRotator(-Elevation, Azimuth, 0.0);
	Out.TemperatureKelvin = FMath::Lerp(DuskTemperatureKelvin, NoonTemperatureKelvin, static_cast<float>(Alpha));
	Out.Intensity = FMath::Lerp(NoonIntensity * DuskIntensityFraction, NoonIntensity, static_cast<float>(Alpha));
	return Out;
}
