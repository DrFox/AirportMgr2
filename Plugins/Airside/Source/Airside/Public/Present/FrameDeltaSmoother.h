#pragma once

#include "CoreMinimal.h"

/**
 * Turns a jittering real frame delta into the even step the display will present.
 *
 * A PLAIN STRUCT, deliberately: this used to be four UPROPERTYs and a private method on
 * ARoadNetworkActor (issue #80 - the composition root growing an algorithm nobody owned).
 * Moving the state AND the logic into one value type is what lets
 * Airside.Present.FrameDeltaSmoother pin it with no World, no actor and no UObject at all -
 * see that test for the two fixes below measured directly, and
 * Airside.Present.EvenStepsUnderAJitteringFrameRate for the same algorithm proven through a
 * real ARoadNetworkActor::Tick.
 *
 * WHY THE DELTA IS EVENED AT ALL. The display presents frames on a fixed cadence - vsync
 * holds each one for a whole number of refreshes - while DeltaSeconds is real measured
 * wall-clock and jitters by several percent frame to frame. An agent advancing
 * Speed x DeltaSeconds therefore covers a DIFFERENT distance in each equally-long display
 * slot, and that is a velocity flicker, not a position error: measured from a play session
 * on 2026-09-12, mean 5.1%, p95 16.8%, worst 36%.
 *
 * It matches every part of the report. The error is a fixed distance in world space, so it
 * grows on screen as the camera closes in. It scales with speed - 1.64 uu a frame on a
 * landing rollout against 0.14 uu in a tight turn - so a landing judders and a slow corner
 * does not. And it is invisible to every test of the model, because the model is right: the
 * aircraft really is where it says it is, at a time that is not evenly spaced.
 *
 * SmoothingRate and MaxOwedSeconds are NOT members here. They are ARoadNetworkActor's own
 * level-authored UPROPERTYs and travel into Advance BY VALUE on every call, the same pattern
 * FTrafficRules uses reaching UAirsideTraffic::Advance - a level-tuned figure copied in each
 * frame cannot go stale against a PIE duplication the way a value cached in here could.
 */
struct FFrameDeltaSmoother
{
	/** Running average of the frame delta, seconds. See SmoothingRate. Zero means
	 *  "not yet seeded" - the very first call seeds it from its own RawDeltaSeconds rather
	 *  than from zero, so the first second of play is not an aeroplane accelerating out of a
	 *  standstill the model never asked for. */
	double SmoothedSeconds = 0.0;

	/** Simulated time owed to the wall clock, positive when behind. See MaxOwedSeconds. */
	double OwedSeconds = 0.0;

	/**
	 * Turns RawDeltaSeconds into the evened step the display will present, and updates both
	 * members above in the process.
	 *
	 * HITCH EXCLUSION (2026-09-13). A frame many times the running average - an editor
	 * hitch, an asset load, PIE's own long first frame - used to be Lerp'd straight into
	 * SmoothedSeconds like any other sample, which is backwards: that is exactly the kind of
	 * frame the average exists to smooth OVER, not learn from. One measured 0.4 s hitch
	 * against a ~0.008 s baseline used to take ~22 frames to decay back under 1.5x normal.
	 * A frame this far outside the average still banks its real time into OwedSeconds below -
	 * nothing is lost - it just does not get to move the average.
	 *
	 * OWED-BANK DECAY (2026-09-13). OwedSeconds used to be paid out only by Step below, and
	 * Step tracks SmoothedSeconds closely by design - so once a hitch pushed the bank to the
	 * MaxOwedSeconds clamp, nothing ever pulled it back: a long first PIE frame pinned Owed at
	 * -MaxOwedSeconds for the rest of the session. A proportional decay every call, applied
	 * before the clamp, guarantees the bank actually empties instead of merely being stopped
	 * from growing further.
	 */
	double Advance(double RawDeltaSeconds, double SmoothingRate, double MaxOwedSeconds)
	{
		if (RawDeltaSeconds <= 0.0)
		{
			return 0.0;
		}

		const double Rate = FMath::Clamp(SmoothingRate, 0.0, 1.0);
		if (SmoothedSeconds <= 0.0)
		{
			SmoothedSeconds = RawDeltaSeconds;
		}

		// Excluded from the average, not from the bank - see "HITCH EXCLUSION" above.
		constexpr double HitchMultiple = 4.0;
		if (RawDeltaSeconds <= SmoothedSeconds * HitchMultiple)
		{
			SmoothedSeconds = FMath::Lerp(SmoothedSeconds, RawDeltaSeconds, Rate);
		}

		// WHAT IS OWED, so evening the step cannot turn into losing time. The average is
		// paid out each call and the difference banked; the clamp below is what stops the
		// bank growing, and the decay is what makes it actually shrink - see "OWED-BANK
		// DECAY" above.
		OwedSeconds += RawDeltaSeconds;
		constexpr double OwedDecayRate = 0.1;
		OwedSeconds -= OwedSeconds * OwedDecayRate;

		const double Bound = FMath::Max(MaxOwedSeconds, 0.0);
		const double Step = FMath::Max(
			FMath::Clamp(SmoothedSeconds, OwedSeconds - Bound, OwedSeconds + Bound), 0.0);

		OwedSeconds -= Step;
		return Step;
	}
};
