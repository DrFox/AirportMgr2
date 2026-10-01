#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AirsideTestsLog.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"

// MODEL-LAYER EXIT ARC TESTS, against the SAME FExitArcAirport fixture RunwayExitArcTest.cpp's
// Build tests use - issue #105 item 13 split the one 800-line RunwayExitArcTest.cpp into
// these two files, hoisting the shared fixture and node/turn finders into
// AirsideTestFixtures.h so neither file needs the other.

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE REPORT OF 2026-09-06: "rolls out to the exit, stops (immediately to 0 m/s), then
 * seems to respawn facing the exit route and accelerates along the taxiway". Two motion
 * models met at a point and neither carried anything across. This measures the whole
 * ground run - touchdown to parked, the handover frame included - and asserts no speed or
 * heading step larger than the airframe could make in one tick.
 *
 * Red on the old handover twice over: speed 800 -> 0 (Start reset it) and heading by the
 * taxiway's 45 degrees (seeded from a straight stub).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficVacatedHandoverIsContinuousTest,
	"Airside.Model.Traffic.VacatedHandoverIsContinuous",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficVacatedHandoverIsContinuousTest::RunTest(const FString& Parameters)
{
	FExitArcAirport A = ExitArcBuildAirport(GetTransientPackage(), /*bWithStand=*/true);
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe, 10.0);
	if (!TestTrue(TEXT("the arrival is dispatched"), Id > 0)) { return false; }

	constexpr double Dt = 1.0 / 60.0;
	// What one tick may change, with half again for the frame the handover spends twice.
	const double SpeedStepAllowed = FMath::Max3(Airframe.Chassis.Ground.Landing.Decel, Airframe.Chassis.Ground.Taxi.Accel,
		Airframe.Chassis.Ground.Taxi.Decel) * Dt * 1.5 + 1.0;
	// WHAT ONE TICK MAY TURN IS THE STEER LAW'S (#449). A pivoting airframe turns at MaxTurnRateDegPerSec flat; a
	// rolling-steer one at v sin(lock) / L about its steered axle, and v tan(lock) / L on the final turn, which pivots
	// about the fixed axle (TightestReversibleRadius's reason) - so tan, the larger, bounds both. The default airframe
	// was a hand copy with no wheelbase until #449 and pivoted; the Meridian it is now rolls. Restated here rather than
	// read off the chassis helpers, for TightestFollowableRadius's reason. Per tick, at the speed measured then.
	const bool bRolls = Airframe.Chassis.EffectiveSteerLaw() == ESteerLaw::RollingSteer;
	const double LockTan = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(Airframe.Chassis.Ground.MaxSteerDegrees, 0.0, 89.0)));
	auto HeadingStepAllowedAt = [&](double AtSpeed)
	{
		const double Rate = bRolls ? AtSpeed * LockTan / FMath::Max(Airframe.Chassis.Wheelbase(), 1.0)
			: FMath::DegreesToRadians(Airframe.Chassis.Ground.MaxTurnRateDegPerSec);
		return Rate * Dt * 1.5 + 1.0e-4;
	};
	// THE LAW'S BOUND IS LOOSE AT TAXI SPEED - 7 degrees a tick at 1000 uu/s (#477 review) against steps of about 0.1 -
	// so it cannot see the defect this test exists for, a heading mis-seeded at the phase flip. The HANDOVER FRAME gets
	// its own bound: the first Taxiing tick after Arriving turns no more than half a degree. A seed off by a degree or
	// two shows as that whole jump, because a rolling-steer follower takes an error out at v sin(error) / L, a few
	// percent of it a tick.
	constexpr double HandoverStepAllowedDeg = 0.5;

	bool bHadGround = false;
	FVector2D PrevAt = FVector2D::ZeroVector;
	double PrevHeading = 0.0;
	double PrevSpeed = -1.0;
	EAgentPhase PrevPhase = EAgentPhase::Arriving;
	double WorstSpeedStep = 0.0, WorstSpeedAt = 0.0;
	// TWO WORSTS, both logged (#477 review): the largest step outright, and the step closest to its own tick's bound.
	double LargestHeadingStep = 0.0, LargestHeadingAt = 0.0;
	bool bHaveTightest = false;
	double TightestMargin = 0.0, TightestStep = 0.0, TightestAt = 0.0, TightestAllowed = 0.0;
	double HandoverStep = -1.0, HandoverAt = 0.0;
	bool bSawTaxi = false, bParked = false;
	int32 Ticks = 0;
	for (; Ticks < 600 * 60; ++Ticks)
	{
		Traffic->Advance(Dt, A.Net);
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		if (Agent == nullptr) { break; }
		if (Agent->Phase == EAgentPhase::Parked) { bParked = true; break; }
		bSawTaxi = bSawTaxi || Agent->Phase == EAgentPhase::Taxiing;

		const FAgentMotion& M = Agent->LastMotion;
		if (M.Altitude > 0.0)
		{
			bHadGround = false;   // airborne: nothing to compare across yet
			continue;
		}
		const double Speed = bHadGround ? FVector2D::Distance(M.Position, PrevAt) / Dt : -1.0;
		if (bHadGround && PrevSpeed >= 0.0)
		{
			const double SpeedStep = FMath::Abs(Speed - PrevSpeed);
			const double HeadingStep = FMath::Abs(FMath::UnwindRadians(M.Heading - PrevHeading));
			if (SpeedStep > WorstSpeedStep) { WorstSpeedStep = SpeedStep; WorstSpeedAt = Ticks * Dt; }
			if (HeadingStep > LargestHeadingStep) { LargestHeadingStep = HeadingStep; LargestHeadingAt = Ticks * Dt; }
			// THE TIGHTEST AGAINST ITS OWN TICK'S BOUND, the faster of the two speeds either side of the step. A FLAG, not
			// a zero test: a record of a zero step was overwritten by every later tick, and logged the last one (#477 review).
			const double AllowedHere = HeadingStepAllowedAt(FMath::Max(Speed, PrevSpeed));
			const double Margin = AllowedHere - HeadingStep;
			if (!bHaveTightest || Margin < TightestMargin)
			{
				bHaveTightest = true;
				TightestMargin = Margin; TightestStep = HeadingStep; TightestAt = Ticks * Dt; TightestAllowed = AllowedHere;
			}
			if (HandoverStep < 0.0 && PrevPhase == EAgentPhase::Arriving && Agent->Phase == EAgentPhase::Taxiing)
			{
				HandoverStep = HeadingStep;
				HandoverAt = Ticks * Dt;
			}
		}
		// THE HANDOVER, tick by tick, so a failure is read off the numbers: the frame the
		// phase flips and the two either side of it.
		if (Agent->Phase == EAgentPhase::Taxiing && Agent->Follower.Travelled < 60.0)
		{
			UE_LOG(LogAirsideTests, Log,
				TEXT("t=%.3f phase %d at (%.1f, %.1f) hdg %.2f deg measured %.1f uu/s; follower travelled %.1f speed %.1f; rollout travelled %.1f speed %.1f"),
				Ticks * Dt, static_cast<int32>(Agent->Phase), M.Position.X, M.Position.Y,
				FMath::RadiansToDegrees(M.Heading), Speed,
				Agent->Follower.Travelled, Agent->Follower.Speed, Agent->Arrival.Travelled, Agent->Arrival.Speed);
		}
		else if (Agent->Phase == EAgentPhase::Arriving && Agent->Arrival.Travelled > Agent->Arrival.VacateAt - 40.0)
		{
			UE_LOG(LogAirsideTests, Log,
				TEXT("t=%.3f phase %d at (%.1f, %.1f) hdg %.2f deg measured %.1f uu/s; rollout travelled %.1f of %.1f speed %.1f"),
				Ticks * Dt, static_cast<int32>(Agent->Phase), M.Position.X, M.Position.Y,
				FMath::RadiansToDegrees(M.Heading), Speed,
				Agent->Arrival.Travelled, Agent->Arrival.VacateAt, Agent->Arrival.Speed);
		}
		PrevAt = M.Position;
		PrevHeading = M.Heading;
		PrevSpeed = Speed;
		PrevPhase = Agent->Phase;
		bHadGround = true;
	}

	UE_LOG(LogAirsideTests, Log,
		TEXT("Handover measured over %d ticks: worst speed step %.1f uu/s per tick at %.2f s (allowed %.1f); largest heading step %.3f deg at %.2f s; ")
		TEXT("tightest heading step %.3f deg at %.2f s against %.3f allowed (margin %.3f); handover-frame step %.3f deg at %.2f s (allowed %.1f); parked %d"),
		Ticks, WorstSpeedStep, WorstSpeedAt, SpeedStepAllowed,
		FMath::RadiansToDegrees(LargestHeadingStep), LargestHeadingAt,
		FMath::RadiansToDegrees(TightestStep), TightestAt, FMath::RadiansToDegrees(TightestAllowed), FMath::RadiansToDegrees(TightestMargin),
		FMath::RadiansToDegrees(HandoverStep), HandoverAt, HandoverStepAllowedDeg, bParked);

	TestTrue(TEXT("the aircraft taxied after landing"), bSawTaxi);
	TestTrue(TEXT("and parked within ten minutes"), bParked);
	TestTrue(FString::Printf(TEXT("no speed step beyond one tick's braking or acceleration (worst %.1f uu/s at %.2f s, allowed %.1f)"),
			WorstSpeedStep, WorstSpeedAt, SpeedStepAllowed),
		WorstSpeedStep <= SpeedStepAllowed);
	TestTrue(FString::Printf(TEXT("no heading step beyond one tick's turn (tightest %.3f deg at %.2f s, allowed %.3f)"),
			FMath::RadiansToDegrees(TightestStep), TightestAt, FMath::RadiansToDegrees(TightestAllowed)),
		bHaveTightest && TightestMargin >= 0.0);
	if (TestTrue(TEXT("the handover frame was measured"), HandoverStep >= 0.0))
	{
		TestTrue(FString::Printf(TEXT("the heading carries across the phase flip (%.3f deg on the first taxiing tick, allowed %.1f)"),
				FMath::RadiansToDegrees(HandoverStep), HandoverStepAllowedDeg),
			FMath::RadiansToDegrees(HandoverStep) <= HandoverStepAllowedDeg);
	}
	return true;
}

/**
 * THE AIRCRAFT THAT ROLLED STRAIGHT PAST ITS EXIT (PIE, 2026-09-06, the first build with
 * arcs). The exit arc began between the distance the aircraft is actually slowed by and the
 * margined "needed" figure, so it was ruled unusable; the junction's own node-end, 6000 uu
 * further on, was not - and from there the only way off is along the runway to the next
 * split and back through the hairpin. Two rules, both measured here: usability is judged at
 * the raw slowed-by distance, and a node whose route begins along the strip is no exit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalTakesTheArcNotTheJunctionTest,
	"Airside.Model.ArrivalTakesTheArcNotTheJunction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalTakesTheArcNotTheJunctionTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const double Raw = FLandingRun::RequiredLandingDistance(Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach);
	const double Needed = Raw * FLandingRun::LandingMargin;
	// The junction 3000 past the margined figure: its arc starts 3000 SHORT of it, and well
	// past the raw one.
	FExitArcAirport A = ExitArcBuildAirport(GetTransientPackage(), /*bWithStand=*/true, Needed + 3000.0);
	const double ArcStart = Needed + 3000.0 - A.ExitLength;
	TestTrue(FString::Printf(TEXT("fixture: arc start %.0f lies between slowed-by %.0f and needed %.0f"), ArcStart, Raw, Needed),
		ArcStart > Raw && ArcStart < Needed);

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	if (!TestTrue(FString::Printf(TEXT("the arrival is planned: %s"), *ArrivalPlanner::DescribeRefusal(Plan)), Plan.IsValid()))
	{
		return false;
	}
	double Miss = 0.0;
	const FGuidelineNodeId SUp = ExitArcNodeNear(*A.Net, A.XAt - FVector2D(A.ExitLength, 0.0), Miss);
	TestTrue(TEXT("the upstream arc start exists"), Miss < 1.0);
	TestTrue(FString::Printf(TEXT("the exit is the arc start (vacating at %.0f, arc start %.0f, junction %.0f)"),
			Plan.VacateAt, ArcStart, Needed + 3000.0),
		Plan.Exit == SUp && FMath::Abs(Plan.VacateAt - ArcStart) < 1.0);
	// MOVED IN FROM Airside.Model.ArrivalExitAtArcStart (#462, merge M10), which asserted the same exit at the
	// same helper and this one's harder case is the stronger place for it: the taxi-in BEGINS ON THE CENTRELINE
	// at that exit - its first polyline point is the exit node, not a point short of it that a hand-off would snap.
	TestTrue(TEXT("and the taxi-in begins on the centreline there"),
		Plan.TaxiIn.Polyline.Num() > 1
		&& FVector2D::Distance(Plan.TaxiIn.Polyline[0], A.Net->GetGuidelineNode(SUp)->Position) < 1.0);

	bool bAlongRunway = false;
	for (const FRouteStep& Step : Plan.TaxiIn.Steps)
	{
		const FGuidelineEdge* Edge = A.Net->GetGuidelineEdge(Step.Edge);
		bAlongRunway = bAlongRunway || (Edge && Edge->DerivedFrom.IsSet() && A.Net->IsRunwaySegment(Edge->DerivedFrom));
	}
	TestFalse(TEXT("the taxi-in never runs along the runway - it turns off, forward, at the arc"), bAlongRunway);
	TestTrue(TEXT("and its first span heads down the runway, not back"),
		Plan.TaxiIn.Polyline.Num() > 1
		&& FVector2D::DotProduct(Plan.TaxiIn.Polyline[1] - Plan.TaxiIn.Polyline[0], FVector2D(1.0, 0.0)) > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalRefusedWhenOnlyStandInStripTest,
	"Airside.Model.ArrivalRefusedWhenOnlyStandInStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalRefusedWhenOnlyStandInStripTest::RunTest(const FString& Parameters)
{
	// THE FIELD'S ONLY STAND FITS AND IS PAVED, but its back corner sits 35 m from the diagonal
	// taxiway's dead-end tip - inside that taxiway's 40 m reach. The fix is "redraw it back",
	// and the refusal must say so, not "pave a stand": WhyEveryStandRefused's strip-first
	// priority (review, 2026-09-28). The spot is where ExitArcBuildAirport's stand stood
	// before the strip moved it.
	FExitArcAirport A = ExitArcBuildAirport(GetTransientPackage(), /*bWithStand=*/false);
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	A.Net->PlaceEntity(Stand, Stand->Anchors, A.XAt + FVector2D(25000.0, -14000.0), 0.0);
	FAnchorLink::Build(*A.Net, UAirsideSettings::ResolveLargestServiceVehicle());

	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FArrivalPlan Plan = ArrivalPlanner::Plan(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	TestEqual(TEXT("refused for the strip"), Plan.Why, EArrivalRefusal::NoStandClearOfStrip);
	TestTrue(FString::Printf(TEXT("and the sentence says to redraw (said: %s)"), *ArrivalPlanner::DescribeRefusal(Plan)),
		ArrivalPlanner::DescribeRefusal(Plan).Contains(TEXT("redraw")));
	return true;
}

#endif
