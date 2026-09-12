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

	/** The dusk floor: the sun never goes below this. See the struct comment. */
	double MinElevationDegrees = 8.0;

	/** Compass bearing the sun sits at when it is at MaxElevationDegrees. */
	double NoonAzimuthDegrees = 150.0;

	float NoonTemperatureKelvin = 5800.0f;

	/** Warmer at the floor. Warmth is most of what sells time of day. */
	float DuskTemperatureKelvin = 3200.0f;

	/** The engine's own directional default is 10, and Slice A leaves it there. */
	float NoonIntensity = 10.0f;

	/**
	 * Floor intensity as a fraction of noon.
	 *
	 * Once exposure is locked to a daylight EV this decides whether dusk is playable, so it
	 * is the figure most likely to move after the first screenshot of a sunset.
	 */
	float DuskIntensityFraction = 0.35f;

	/**
	 * The sun at DayFraction, where 0 is midnight and 0.5 is noon.
	 *
	 * Values outside [0, 1) are wrapped, so a caller may hand over a raw clock reading
	 * without reducing it first.
	 */
	FSunLighting At(double DayFraction) const;
};
