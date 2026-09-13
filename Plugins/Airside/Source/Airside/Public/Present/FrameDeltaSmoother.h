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
	 * HITCH WINSORISING (2026-09-13, revised 2026-09-13 in code review). A frame many times
	 * the running average - an editor hitch, an asset load, PIE's own long first frame - used
	 * to be Lerp'd straight into SmoothedSeconds like any other sample, which is backwards:
	 * that is exactly the kind of frame the average exists to smooth OVER, not learn from. One
	 * measured 0.4 s hitch against a ~0.008 s baseline used to take ~22 frames to decay back
	 * under 1.5x normal.
	 *
	 * NOT excluded outright, though - the first version of this fix skipped the Lerp entirely
	 * whenever a frame was too far above the average, which review caught as its own bug: a
	 * SUSTAINED rise (dropping from 120 Hz to a steady 30 fps) is also "too far above the old
	 * average" on every single frame, so the average never re-learns it, and the sim keeps
	 * running as if still at 120 Hz for good. Instead the value FED to the Lerp is capped at
	 * HitchMultiple times the current average - a one-frame spike is heavily dampened (one
	 * 0.4 s hitch moves the average by 1.3x, not by the frame's full 50x) but a sustained rise
	 * still pulls the cap up with it every frame and the average catches up geometrically
	 * within a couple of hundred frames, same as an ordinary EMA. This also means the fix does
	 * NOT reach a huge FIRST frame's seeding above - see the field comment on SmoothedSeconds -
	 * but that is what OWED-BANK RECOVERY below is for.
	 *
	 * OWED-BANK RECOVERY (2026-09-13, revised 2026-09-13 in code review). Without any of this,
	 * a hitch (or a huge first frame, which SEEDS SmoothedSeconds directly and is not subject
	 * to the winsorising above) pushes OwedSeconds to the MaxOwedSeconds clamp and it stays
	 * there: paying out SmoothedSeconds every frame moves Owed by (RawDeltaSeconds -
	 * SmoothedSeconds), and while the clamp is engaged that difference is exactly what keeps
	 * Owed pinned at the clamp - a long first PIE frame pinned it there for the rest of the
	 * session.
	 *
	 * The first fix decayed OwedSeconds by a fixed fraction every call, BEFORE computing Step -
	 * which review also caught: while the clamp is engaged, Step is defined as
	 * (post-decay Owed + Bound), so decaying Owed first does not shrink the bank at all, it
	 * INFLATES Step by the same amount the decay just removed, handing the sim extra time
	 * every single frame the bank is being paid down (measured: 300 frames of a real 2.4 s
	 * following a 2.0 s hitch delivered 3.68 sim-seconds, not 2.4). The fix decays Owed only on
	 * a frame where the clamp did NOT engage - i.e. Step already equalled the smoothed rate
	 * unclamped - which cannot perturb Step (Step is not computed from the decayed value on
	 * those frames) and still lets a genuinely near-zero bank bleed off any residual rather
	 * than drifting on jitter alone. The clamped recovery itself needs no help: with Owed
	 * pinned at the clamp, Step already equals RawDeltaSeconds exactly every frame (real-time
	 * passthrough, provably neither fast nor slow), and the pin releases on its own once
	 * SmoothedSeconds decays close enough to the live average for the clamp to stop engaging.
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
			// Not winsorised: there is nothing to cap against yet. See "OWED-BANK RECOVERY".
			SmoothedSeconds = RawDeltaSeconds;
		}
		else
		{
			// Capped, not dropped - see "HITCH WINSORISING" above for why a sustained rise
			// must still move this, just not by a single frame's full amount. A CODE
			// CONSTANT, not a tunable - unlike SmoothingRate/MaxOwedSeconds above, nothing
			// about "how many multiples of the average counts as a hitch" is per-airport.
			constexpr double HitchMultiple = 4.0;
			const double Capped = FMath::Min(RawDeltaSeconds, SmoothedSeconds * HitchMultiple);
			SmoothedSeconds = FMath::Lerp(SmoothedSeconds, Capped, Rate);
		}

		// WHAT IS OWED, so evening the step cannot turn into losing time. The average is paid
		// out each call and the difference banked; the clamp below is what stops the bank
		// growing further.
		const double OwedBeforeStep = OwedSeconds + RawDeltaSeconds;
		const double Bound = FMath::Max(MaxOwedSeconds, 0.0);
		const double Unclamped = FMath::Max(SmoothedSeconds, 0.0);
		const double BoundedByOwed = FMath::Clamp(Unclamped, OwedBeforeStep - Bound, OwedBeforeStep + Bound);
		const double Step = FMath::Max(BoundedByOwed, 0.0);

		OwedSeconds = OwedBeforeStep - Step;

		// Only when the owed-bound clamp did not just decide Step FOR us - see "OWED-BANK
		// RECOVERY" for why decaying a clamped frame's bank only inflates the very step it was
		// meant to shrink. Checked against BoundedByOwed (before the final floor at zero, which
		// is a different, much rarer clamp) with FMath::IsNearlyEqual, not ==: both sides come
		// through FP arithmetic that need not land on the same bit pattern even when neither
		// bound was the binding constraint.
		if (FMath::IsNearlyEqual(BoundedByOwed, Unclamped, 1e-9))
		{
			// Also a CODE CONSTANT: how fast a residual bleeds off is an implementation
			// detail of the smoothing, not a per-airport figure.
			constexpr double OwedDecayRate = 0.1;
			OwedSeconds -= OwedSeconds * OwedDecayRate;
		}

		return Step;
	}
};
