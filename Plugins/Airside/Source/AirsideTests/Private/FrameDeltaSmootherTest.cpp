#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Present/FrameDeltaSmoother.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------
/**
 * WORLD-FREE, deliberately: no UWorld, no ARoadNetworkActor, no UObject at all - just the
 * struct issue #80 pulled off the actor. See FFrameDeltaSmoother's own header for the WHY of
 * the algorithm and of the two fixes this test pins; Airside.Present.
 * EvenStepsUnderAJitteringFrameRate proves the same algorithm again through a real
 * ARoadNetworkActor::Tick, which is the composition-level half this one is not trying to be.
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

	// --- Fix 1: a hitch frame must not pollute the learned average --------------------
	{
		FFrameDeltaSmoother Smoother;
		for (int32 I = 0; I < 60; ++I) { Smoother.Advance(Baseline, Rate, MaxOwed); }
		const double SmoothedBeforeHitch = Smoother.SmoothedSeconds;
		TestTrue(TEXT("warmed up to the steady baseline"),
			FMath::IsNearlyEqual(SmoothedBeforeHitch, Baseline, 1e-6));

		// One 0.4s hitch - 50x the baseline - the exact figure from the 2026-09-12 report.
		Smoother.Advance(0.4, Rate, MaxOwed);

		TestTrue(*FString::Printf(
			TEXT("a single hitch does not drag the average up (was %.6f, is %.6f)"),
			SmoothedBeforeHitch, Smoother.SmoothedSeconds),
			Smoother.SmoothedSeconds < SmoothedBeforeHitch * 1.5);

		// Unfixed, this used to take ~22 frames of steady baseline to fall back under 1.5x -
		// the very thing excluding the hitch from the EMA makes unnecessary: it never rose.
		Smoother.Advance(Baseline, Rate, MaxOwed);
		TestTrue(TEXT("and the very next frame is already back at (or under) baseline"),
			Smoother.SmoothedSeconds <= SmoothedBeforeHitch * 1.5);
	}

	// --- Fix 2: the owed bank must not stay pinned at the clamp ------------------------
	{
		FFrameDeltaSmoother Smoother;
		// PIE's own long first frame - big enough to drive Owed straight to -MaxOwed.
		Smoother.Advance(2.0, Rate, MaxOwed);
		TestTrue(*FString::Printf(TEXT("the big first frame banks the clamp (Owed=%.4f)"), Smoother.OwedSeconds),
			Smoother.OwedSeconds < -MaxOwed * 0.75);

		for (int32 I = 0; I < 20; ++I) { Smoother.Advance(Baseline, Rate, MaxOwed); }
		TestTrue(*FString::Printf(TEXT("twenty frames in, still recovering (Owed=%.4f)"), Smoother.OwedSeconds),
			Smoother.OwedSeconds < -MaxOwed * 0.5);

		for (int32 I = 0; I < 300; ++I) { Smoother.Advance(Baseline, Rate, MaxOwed); }
		TestTrue(*FString::Printf(
			TEXT("and it actually returns near zero rather than staying pinned (Owed=%.4f, MaxOwed=%.2f)"),
			Smoother.OwedSeconds, MaxOwed),
			FMath::Abs(Smoother.OwedSeconds) < MaxOwed * 0.25);
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
