#pragma once

#include "CoreMinimal.h"

/** Everything the sun driver sets on the directional light for one instant. */
struct FSunLighting
{
	FRotator Rotation = FRotator::ZeroRotator;

	float TemperatureKelvin = 5800.0f;

	/** Matches ULightComponent::Intensity for a directional light. */
	float Intensity = 10.0f;
};

/**
 * Maps a time of day to where the sun is and what colour it is.
 *
 * A PLAIN STRUCT - no UObject, no world, no knowledge of lights, actors or clocks. Same
 * shape and same reason as FBuildCameraRig: it computes values from numbers, which is what
 * makes the curve unit-testable with no level. ASunDriver holds the tunables as UPROPERTYs
 * and copies them on, exactly as ARoadBuildController::ApplyViewLimits does for the rig.
 *
 * THE ARC IS FLOORED ABOVE THE HORIZON, and that is the design rather than a
 * simplification. USimClock::RealSecondsPerGameDay is 1200, so a literal sun sweeps 18
 * degrees a minute at x1 and 144 at x8, and roughly 40% of a day is dark. With no runway
 * lights, no apron floods and exposure locked, that dark is unreadable rather than
 * atmospheric - a defect, not a mood. The concept sheet agrees: its lighting mast is
 * captioned "For when you operate later", so night operations are a progression unlock.
 * When those lights exist, the floor lifts and night becomes the unlock.
 *
 * Between 18:00 and 06:00 the sun therefore sits at the floor while its azimuth carries on
 * round to the north, which reads as a long high-latitude twilight - a real thing the sky
 * does, not a glitch.
 */
struct FSunPath
{
	/** Elevation at noon. The angle the art direction was judged against. */
	double MaxElevationDegrees = 42.0;

	/**
	 * The dusk floor: the sun never goes below this. See the struct comment for why there
	 * is a floor at all.
	 *
	 * THIS, NOT DuskIntensityFraction, IS THE LEVER THAT SETS HOW DARK NIGHT GETS, because
	 * ground illumination scales with the SINE of elevation. At 8 degrees the sun delivers
	 * sin(8)/sin(42) = 21% of its noon contribution before the fraction is even applied, so
	 * no fraction can rescue a grazing sun.
	 *
	 * Measured in PIE against the daylight ground (mean sRGB #86862F, luminance 0.2232):
	 *
	 *    8 deg / 0.35  ->  #341A00,  3.93 stops below noon - near black AND sepia
	 *   22 deg / 0.55  ->  #6D5D0E,  1.01 stops           - barely reads as night at all
	 *   15 deg / 0.45  ->  #503F05,  2.10 stops           - dark, readable, clearly night
	 *
	 * 15 and 0.45 are those measurements, not a guess. Two stops is the band where the
	 * field still reads as a field and the player can still see what they are building,
	 * which is the whole bargain the floor exists to strike.
	 */
	double MinElevationDegrees = 15.0;

	/** Compass bearing the sun sits at when it is at MaxElevationDegrees. */
	double NoonAzimuthDegrees = 150.0;

	float NoonTemperatureKelvin = 5800.0f;

	/**
	 * Warmer at the floor, but only mildly - 4300, not the 3200 this started at.
	 *
	 * Warmth sells a SUNSET, and 3200 K is tungsten. The catch is that on a floored arc
	 * the floor is held all night, so a sunset temperature becomes the colour of midnight:
	 * measured in PIE at 3200 K the grass rendered brown (#341A00) with no green left in
	 * it, which reads as sepia rather than as night.
	 *
	 * A single elevation-to-temperature mapping cannot tell sunset from midnight, because
	 * on this curve they are the same elevation. Real night is BLUE - sky-dominated - so
	 * the honest fix is a second curve on the day fraction. Until that exists, 4300 K is
	 * the compromise: still warmer than noon, not orange enough to stain the ground.
	 */
	float DuskTemperatureKelvin = 4300.0f;

	/** The engine's own directional default is 10, and Slice A leaves it there. */
	float NoonIntensity = 10.0f;

	/**
	 * Floor intensity as a fraction of noon.
	 *
	 * Once exposure is locked to a daylight EV this decides whether dusk is playable, so it
	 * is the figure most likely to move after the first screenshot of a sunset.
	 */
	float DuskIntensityFraction = 0.45f;

	/**
	 * The sun at DayFraction, where 0 is midnight and 0.5 is noon.
	 *
	 * Values outside [0, 1) are wrapped, so a caller may hand over a raw clock reading
	 * without reducing it first.
	 */
	FSunLighting At(double DayFraction) const;
};
