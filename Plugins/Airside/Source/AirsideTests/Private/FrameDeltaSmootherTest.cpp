#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Present/FrameDeltaSmoother.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------
/**
 * WORLD-FREE, deliberately: no UWorld, no ARoadNetworkActor, no UObject at all - just the
 * struct issue #80 pulled off the actor. See FFrameDeltaSmoother's own header for the WHY of
 * the algorithm and of the two fixes this test pins (both revised once more in code review -
 * see the header's "2026-09-13 in code review" notes for what the first version of each got
 * wrong); Airside.Present.EvenStepsUnderAJitteringFrameRate proves the same algorithm again
 * through a real ARoadNetworkActor::Tick, which is the composition-level half this one is not
 * trying to be.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFrameDeltaSmootherTest,
	"Airside.Present.FrameDeltaSmoother",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFrameDeltaSmootherTest::RunTest(const FString& Parameters)
{
	const double Rate = 0.1;
	const double MaxOwed = 0.25;
	const double Baseline = 0.008; // ~120 Hz, the same order SimTimeScaleTest's jitter fixture uses.

	// --- Fix 1a: a single hitch frame is dampened, not learned wholesale ---------------
	{
		FFrameDeltaSmoother Smoother;
		for (int32 I = 0; I < 60; ++I) { Smoother.Advance(Baseline, Rate, MaxOwed); }
		const double SmoothedBeforeHitch = Smoother.SmoothedSeconds;
		TestTrue(TEXT("warmed up to the steady baseline"),
			FMath::IsNearlyEqual(SmoothedBeforeHitch, Baseline, 1e-6));

		// One 0.4s hitch - 50x the baseline - the exact figure from the 2026-09-12 report.
		Smoother.Advance(0.4, Rate, MaxOwed);

		// WINSORISED, not excluded: the value fed to the Lerp is capped at 4x the running
		// average (0.032), which moves the average by exactly 1.3x - not by the frame's own
		// 50x, and not by zero either. A prior version of this fix excluded the frame
		// entirely, which review caught as its own bug - see Fix 1b below for the case that
		// broke.
		const double Ratio = Smoother.SmoothedSeconds / SmoothedBeforeHitch;
		TestTrue(*FString::Printf(
			TEXT("a single hitch moves the average by ~1.3x, not by its own 50x (ratio %.3f)"), Ratio),
			Ratio > 1.2 && Ratio < 1.4);
	}

	// --- Fix 1b: a SUSTAINED rise must still be learned, not just a single spike -------
	{
		// Caught in code review: the first version of the hitch fix excluded any frame more
		// than 4x the running average from the EMA outright - which also excludes every frame
		// of a genuine, sustained rate drop (120 Hz -> a steady 30 fps is 4x on every single
		// frame), so the average would never re-learn it and the sim would run at ~24% speed
		// for the rest of the session. Winsorising instead of excluding fixes this: the cap
		// itself rises geometrically with the average, so a sustained rise still converges.
		FFrameDeltaSmoother Smoother;
		for (int32 I = 0; I < 200; ++I) { Smoother.Advance(Baseline, Rate, MaxOwed); }

		const double Dropped = 0.0333; // a steady 30 fps.
		double LastStep = 0.0;
		for (int32 I = 0; I < 300; ++I) { LastStep = Smoother.Advance(Dropped, Rate, MaxOwed); }

		TestTrue(*FString::Printf(
			TEXT("300 frames at a steady rate the average re-learns it (Smoothed=%.5f, target=%.5f)"),
			Smoother.SmoothedSeconds, Dropped),
			FMath::Abs(Smoother.SmoothedSeconds / Dropped - 1.0) < 0.05);
		TestTrue(TEXT("and the delivered step tracks the new rate, not the stale one"),
			FMath::Abs(LastStep / Dropped - 1.0) < 0.05);
	}

	// --- Fix 2: the owed bank recovers WITHOUT inflating the step it pays out ----------
	{
		// Caught in code review: the first version of this fix decayed OwedSeconds by a fixed
		// fraction every call, BEFORE computing Step. While the clamp is engaged Step is
		// DEFINED as (post-decay Owed + Bound), so decaying first does not shrink the bank at
		// all - it inflates Step by exactly what the decay removed, handing the sim extra
		// time every frame the bank is being paid down. Measured with the bug: 300 frames of a
		// real 2.4s following a 2.0s hitch delivered 3.68 sim-seconds. The fix decays Owed
		// only on a frame where the clamp did not just decide Step for it.
		FFrameDeltaSmoother Smoother;

		// PIE's own long first frame. This alone banks nothing (Owed stays 0): SEEDING sets
		// SmoothedSeconds to exactly this frame's own value, so Step equals it exactly too -
		// see the "OWED-BANK RECOVERY" comment on why the seed itself is not winsorised. The
		// clamp only engages once REAL frames stop matching that stale seed, on the very next
		// call below.
		Smoother.Advance(2.0, Rate, MaxOwed);
		TestEqual(TEXT("the seed alone delivers exactly what it was given, banking nothing"),
			Smoother.OwedSeconds, 0.0);

		Smoother.Advance(Baseline, Rate, MaxOwed);
		TestTrue(*FString::Printf(TEXT("the very next real frame banks the clamp (Owed=%.4f)"), Smoother.OwedSeconds),
			Smoother.OwedSeconds < -MaxOwed * 0.75);

		double DeliveredSum = Baseline; // the one frame already advanced above.
		for (int32 I = 0; I < 299; ++I) { DeliveredSum += Smoother.Advance(Baseline, Rate, MaxOwed); }
		const double RealElapsed = 300.0 * Baseline;

		// Bounded by MaxOwedSeconds plus ordinary jitter margin, never by however much extra
		// the decay-then-clamp bug was handing out (that measured 1.28s over, not ~0.25s).
		TestTrue(*FString::Printf(
			TEXT("300 real frames (%.3fs) deliver close to that, not inflated by the payoff (delivered %.3fs)"),
			RealElapsed, DeliveredSum),
			FMath::Abs(DeliveredSum - RealElapsed) < MaxOwed + 0.05 * RealElapsed);

		TestTrue(*FString::Printf(
			TEXT("and the bank itself actually returns near zero rather than staying pinned (Owed=%.4f)"),
			Smoother.OwedSeconds),
			FMath::Abs(Smoother.OwedSeconds) < MaxOwed * 0.1);
	}

	// --- A zero or negative raw delta is inert, same as the original EvenDelta --------
	{
		FFrameDeltaSmoother Smoother;
		TestEqual(TEXT("zero raw delta yields zero step"), Smoother.Advance(0.0, Rate, MaxOwed), 0.0);
		TestEqual(TEXT("negative raw delta yields zero step"), Smoother.Advance(-1.0, Rate, MaxOwed), 0.0);
		TestEqual(TEXT("neither touched the running state"), Smoother.SmoothedSeconds, 0.0);
		TestEqual(TEXT("neither touched the owed bank"), Smoother.OwedSeconds, 0.0);
	}

	return true;
}

#endif
