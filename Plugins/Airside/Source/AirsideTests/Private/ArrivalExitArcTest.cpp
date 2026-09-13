#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogArrivalExitArcTest, Log, All);

/**
 * What falls out of the geometry without a planner change, measured rather than assumed:
 * the first strip node past the landing distance that reaches a stand is now the arc's
 * start, so the rollout ends where the taxi begins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalExitAtArcStartTest,
	"Airside.Model.ArrivalExitAtArcStart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalExitAtArcStartTest::RunTest(const FString& Parameters)
{
	FExitArcAirport A = ExitArcBuildAirport(GetTransientPackage(), /*bWithStand=*/true);
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FArrivalPlan Plan = ArrivalPlanner::Plan(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	if (!TestTrue(FString::Printf(TEXT("the arrival is planned: %s"), *ArrivalPlanner::DescribeRefusal(Plan)), Plan.IsValid()))
	{
		return false;
	}
	double Miss = 0.0;
	const FGuidelineNodeId SUp = ExitArcNodeNear(*A.Net, A.XAt - FVector2D(A.ExitLength, 0.0), Miss);
	TestTrue(TEXT("the upstream arc start exists"), Miss < 1.0);
	TestTrue(TEXT("the planner's exit IS the arc start, not the junction node"), Plan.Exit == SUp);
	const double Expected = FVector2D::DotProduct(A.XAt - FVector2D(A.ExitLength, 0.0) - A.Threshold, FVector2D(1.0, 0.0));
	TestTrue(FString::Printf(TEXT("so the rollout vacates at the arc start (%.0f of %.0f)"), Plan.VacateAt, Expected),
		FMath::Abs(Plan.VacateAt - Expected) < 1.0);
	TestTrue(TEXT("and the taxi-in begins on the centreline there"),
		Plan.TaxiIn.Polyline.Num() > 1
		&& FVector2D::Distance(Plan.TaxiIn.Polyline[0], A.Net->GetGuidelineNode(SUp)->Position) < 1.0);
	return true;
}

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
	const double SpeedStepAllowed = FMath::Max3(Airframe.Ground.Landing.Decel, Airframe.Ground.Taxi.Accel,
		Airframe.Ground.Taxi.Decel) * Dt * 1.5 + 1.0;
	const double HeadingStepAllowed = FMath::DegreesToRadians(Airframe.Ground.MaxTurnRateDegPerSec) * Dt * 1.5 + 1.0e-4;

	bool bHadGround = false;
	FVector2D PrevAt = FVector2D::ZeroVector;
	double PrevHeading = 0.0;
	double PrevSpeed = -1.0;
	double WorstSpeedStep = 0.0, WorstHeadingStep = 0.0;
	double WorstSpeedAt = 0.0, WorstHeadingAt = 0.0;
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
			if (HeadingStep > WorstHeadingStep) { WorstHeadingStep = HeadingStep; WorstHeadingAt = Ticks * Dt; }
		}
		// THE HANDOVER, tick by tick, so a failure is read off the numbers: the frame the
		// phase flips and the two either side of it.
		if (Agent->Phase == EAgentPhase::Taxiing && Agent->Follower.Travelled < 60.0)
		{
			UE_LOG(LogArrivalExitArcTest, Log,
				TEXT("t=%.3f phase %d at (%.1f, %.1f) hdg %.2f deg measured %.1f uu/s; follower travelled %.1f speed %.1f; rollout travelled %.1f speed %.1f"),
				Ticks * Dt, static_cast<int32>(Agent->Phase), M.Position.X, M.Position.Y,
				FMath::RadiansToDegrees(M.Heading), Speed,
				Agent->Follower.Travelled, Agent->Follower.Speed, Agent->Arrival.Travelled, Agent->Arrival.Speed);
		}
		else if (Agent->Phase == EAgentPhase::Arriving && Agent->Arrival.Travelled > Agent->Arrival.VacateAt - 40.0)
		{
			UE_LOG(LogArrivalExitArcTest, Log,
				TEXT("t=%.3f phase %d at (%.1f, %.1f) hdg %.2f deg measured %.1f uu/s; rollout travelled %.1f of %.1f speed %.1f"),
				Ticks * Dt, static_cast<int32>(Agent->Phase), M.Position.X, M.Position.Y,
				FMath::RadiansToDegrees(M.Heading), Speed,
				Agent->Arrival.Travelled, Agent->Arrival.VacateAt, Agent->Arrival.Speed);
		}
		PrevAt = M.Position;
		PrevHeading = M.Heading;
		PrevSpeed = Speed;
		bHadGround = true;
	}

	UE_LOG(LogArrivalExitArcTest, Log,
		TEXT("Handover measured over %d ticks: worst speed step %.1f uu/s per tick at %.2f s (allowed %.1f), worst heading step %.3f deg at %.2f s (allowed %.3f), parked %d"),
		Ticks, WorstSpeedStep, WorstSpeedAt, SpeedStepAllowed,
		FMath::RadiansToDegrees(WorstHeadingStep), WorstHeadingAt, FMath::RadiansToDegrees(HeadingStepAllowed), bParked);

	TestTrue(TEXT("the aircraft taxied after landing"), bSawTaxi);
	TestTrue(TEXT("and parked within ten minutes"), bParked);
	TestTrue(FString::Printf(TEXT("no speed step beyond one tick's braking or acceleration (worst %.1f uu/s at %.2f s, allowed %.1f)"),
			WorstSpeedStep, WorstSpeedAt, SpeedStepAllowed),
		WorstSpeedStep <= SpeedStepAllowed);
	TestTrue(FString::Printf(TEXT("no heading step beyond one tick's turn (worst %.3f deg at %.2f s, allowed %.3f)"),
			FMath::RadiansToDegrees(WorstHeadingStep), WorstHeadingAt, FMath::RadiansToDegrees(HeadingStepAllowed)),
		WorstHeadingStep <= HeadingStepAllowed);
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
	const double Raw = FLandingRun::RequiredLandingDistance(Airframe.Ground, Airframe.Climb, Airframe.Approach);
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

#endif
