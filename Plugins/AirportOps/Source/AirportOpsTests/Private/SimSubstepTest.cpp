#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/SimClock.h"
#include "Model/TrafficRules.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE DEFAULT MaxSubsteps CEILING COVERS THE SPEED LADDER, NOT JUST A HITCH (#107 item 4).
 *
 * CONFIRMED, 2026-09-12 review. MaxSubsteps x MaxSubstepSeconds bounds one Advance call at
 * MaxSubsteps * MaxSubstepSeconds - the OLD default of 8 x 1/30 s gave 267 ms - but
 * USimClock's ladder reaches X32, and UAirsideTraffic::Advance's own caller
 * (ARoadNetworkActor::Tick) hands it the real frame time TIMES that multiplier. A 30 fps
 * frame at X32 is 1.067 s of sim time EVERY FRAME, not just on a hitch, which needs 32 steps
 * of MaxSubstepSeconds to stay at the documented target - the old ceiling of 8 clamped that
 * to 8 steps of 133 ms each, four times MaxSubstepSeconds' own 33 ms, which is the same
 * rubber-banding the substep split exists to remove in the first place, just moved to a
 * higher speed setting instead of fixed.
 *
 * HERE, IN AirportOpsTests, SINCE #462: this lived in AirsideTests as Airside.Model.Traffic.
 * SubstepCeilingCoversTheSpeedLadder and passed with its subject broken - the ladder's top
 * was a typed 32.0, so a rung added to USimClock::SpeedLadder (which is in AirportOps, and
 * AirsideTests cannot see it) changed nothing it measured. It also re-derived the step count
 * by hand beside UGroundTraffic::Advance's own clamp. Now the multiplier of EVERY rung is
 * asked of USimClock, and the split is taken by the real Advance and read back from
 * GetLastStepsForTest, so a ladder that grows, a multiplier that changes and a ceiling that
 * shrinks each turn it red.
 *
 * THE LISTS THAT MUST AGREE: USimClock::SpeedLadder (AirportOps) and FTrafficRules::MaxSubsteps
 * (Airside) are two figures with no compile-time link, and this is the one place both are read.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimSubstepCeilingCoversTheSpeedLadderTest,
	"AirportOps.Model.SimClock.SubstepCeilingCoversTheSpeedLadder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimSubstepCeilingCoversTheSpeedLadderTest::RunTest(const FString& Parameters)
{
	// WHAT A LEVEL THAT NEVER TOUCHES THE FIGURES RUNS: the actor's own TrafficRules, which
	// Tick hands to Advance every frame. Read from the CDO rather than a bare FTrafficRules()
	// so an actor constructor that ever overrode the struct default is measured too. No map
	// under Content/ authors MaxSubsteps or MaxSubstepSeconds on an actor instance (a grep of
	// the .umap name tables, 2026-10-01), so the CDO is what every shipped level gets.
	const ARoadNetworkActor* DefaultActor = GetDefault<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("the actor's defaults"), DefaultActor)) { return false; }
	const FTrafficRules& Rules = DefaultActor->TrafficRules;

	UAirsideTraffic* Traffic = NewObject<UAirsideTraffic>(GetTransientPackage());
	if (!TestNotNull(TEXT("traffic constructed"), Traffic)) { return false; }
	UGroundTraffic* Model = Traffic->GetModel();
	if (!TestNotNull(TEXT("and it owns a model"), Model)) { return false; }

	const TArrayView<const ESimSpeed> Ladder = USimClock::SpeedLadder();
	if (!TestTrue(TEXT("the ladder has rungs to cover"), Ladder.Num() > 1)) { return false; }

	// 30 FPS IS THIS CEILING'S OWN FLOOR for ordinary play rather than a hitch (FPropAliasingTest's
	// rate list calls 24 "a hitching" rate and starts bracketing real play at 30), so this is the
	// busiest EVERY-FRAME delta the ladder can ask for, not a hitch outlier the ceiling is allowed
	// to clamp. A DIFFERENT, HIGHER FLOOR (60 fps) is what UAirsideAgentAnim::PropDisplayCapRPM is
	// picked against - the two are chosen separately, one per feature, not read from one shared
	// "ordinary play" constant.
	const double SlowestOrdinaryFrame = 1.0 / 30.0;

	double TopMultiplier = 0.0;
	for (const ESimSpeed Rung : Ladder)
	{
		const double Multiplier = USimClock::Multiplier(Rung);
		TopMultiplier = FMath::Max(TopMultiplier, Multiplier);
		const double SimSeconds = SlowestOrdinaryFrame * Multiplier;

		// THE REAL SPLIT, through the real seam - the same call ARoadNetworkActor::Tick makes
		// with Evened * SimTimeScale, and SimTimeScale is USimClock::Multiplier(Speed)
		// (UOpsRuntime::ApplySpeed, the one production caller of SetSimTimeScale).
		Traffic->Advance(SimSeconds, 0.0, nullptr, Rules);
		const int32 Steps = Model->GetLastStepsForTest();
		const double StepSeconds = SimSeconds / FMath::Max(Steps, 1);

		// THE ASSERTION THE BUG WOULD FAIL: the old default of 8 clamps X32 at 30 fps to 8
		// steps of 133 ms, four times MaxSubstepSeconds, so every frame at that speed - not
		// merely a hitch - was silently taken in steps four times longer than documented. A
		// step is compared with the figure itself, not with a count, so a MaxSubstepSeconds
		// that grew to hide a short ceiling is also seen (a relative 1e-9 absorbs the
		// division's last bit).
		TestTrue(*FString::Printf(
			TEXT("x%.0f at 30 fps (%.3f s of sim time) is taken in steps no longer than MaxSubstepSeconds: "
				"%d steps of %.4f s against %.4f s, ceiling %d"),
			Multiplier, SimSeconds, Steps, StepSeconds, Rules.MaxSubstepSeconds, Rules.MaxSubsteps),
			StepSeconds <= Rules.MaxSubstepSeconds * (1.0 + 1e-9));
	}

	// A LADDER THAT NEVER LEAVES REAL TIME WOULD PASS THE LOOP VACUOUSLY, and the ceiling is
	// sized for the fast end - so the top is named. Asked as "faster than real time", not as a
	// typed 32: the figure is the ladder's to change.
	TestTrue(TEXT("the ladder reaches beyond real time, which is what the ceiling is sized for"), TopMultiplier > 1.0);
	AddInfo(FString::Printf(
		TEXT("SubstepCeilingCoversTheSpeedLadder: ladder top x%.0f at 30 fps; MaxSubsteps %d of MaxSubstepSeconds %.4f s"),
		TopMultiplier, Rules.MaxSubsteps, Rules.MaxSubstepSeconds));
	return true;
}

#endif
