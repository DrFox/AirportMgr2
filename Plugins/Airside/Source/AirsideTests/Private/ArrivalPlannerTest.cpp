#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/LandingRun.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Exit1At (the earlier, longer-taxi exit) is not exposed on FTestAirport - ExitAt is the
	 *  one stands sit beside, exit 2 here - so recomputed the same way FTestAirport::Build
	 *  computes it internally. Used only by EarliestExitWinsTest, to check WHICH junction the
	 *  chosen exit sits at. */
	FVector2D TwoExitFirstExitAt(const FAirframe& Airframe)
	{
		const double Needed = FLandingRun::RequiredLandingDistance(
			Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
		return FVector2D(Needed * 1.2, 0.0);
	}
}

// ---------------------------------------------------------------------------------------
// (a) NoRunway: an empty network has nothing to land on at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerNoRunwayTest,
	"Airside.Model.ArrivalPlanner.NoRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerNoRunwayTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Network, FVector2D::ZeroVector, TestAirframes::Piper());

	TestEqual(TEXT("a network with no runway refuses NoRunway"), Plan.Why, EArrivalRefusal::NoRunway);
	TestFalse(TEXT("and the plan is not valid"), Plan.IsValid());
	return true;
}

// ---------------------------------------------------------------------------------------
// (b) RunwayTooShort: a strip shorter than the landing distance is refused with that
// specific reason, not just a bare false - see CLAUDE.md on "pressing 7 does nothing".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerRunwayTooShortTest,
	"Airside.Model.ArrivalPlanner.RunwayTooShort",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerRunwayTooShortTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const double Needed = FLandingRun::RequiredLandingDistance(
		Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;

	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();

	// Sized from the aircraft, not chosen: a strip under Needed is correctly refused, and a
	// fixture that picked a length out of the air would test the refusal or the acceptance
	// depending on numbers nobody was watching - the same discipline ArrivalDispatchTest uses.
	const FRoadNodeId A = Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Network->AddNode(FVector2D(Needed * 0.4, 0.0));
	Network->AddStraightSegment(A, B, Runway);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Network);
	FRoadGuidelineBuilder::Build(*Network, Solved);

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Network, FVector2D::ZeroVector, Airframe);

	TestEqual(TEXT("a runway shorter than the landing distance refuses RunwayTooShort"),
		Plan.Why, EArrivalRefusal::RunwayTooShort);
	TestEqual(TEXT("and Needed is reported so the refusal can say by how much"), Plan.Needed, Needed);
	return true;
}

// ---------------------------------------------------------------------------------------
// (c) The earliest exit that reaches a stand wins even when a later exit gives a shorter
// taxi - an aircraft takes the first turn-off it can rather than rolling on in search of a
// marginally shorter one. See FTestAirport::Build's ExitCount=2 shape for why BOTH exits can reach the one stand.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerEarliestExitWinsTest,
	"Airside.Model.ArrivalPlanner.EarliestExitWins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerEarliestExitWinsTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Airframe, { .ExitCount = 2 });

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Airport.Net, Airport.Threshold, Airframe);

	if (!TestTrue(TEXT("both exits reach the stand, so the arrival is accepted"), Plan.IsValid()))
	{
		return false;
	}

	// NOT asserted as exactly 2: FRoadGuidelineBuilder gives each segment END its own
	// guideline node, joined to the others at a junction by turn edges rather than merged
	// into one - so a single physical junction with three arms (through-runway plus a
	// taxiway) contributes three entries to RunwayExitNodes, not one. What this test cares
	// about is which PHYSICAL junction the chosen entry sits at, not how many entries a
	// junction happens to produce.
	TestTrue(TEXT("more than one guideline node lies along the runway, from two junctions"),
		Plan.ExitCount > 1);
	TestEqual(TEXT("the chosen entry is the FIRST candidate down the runway"),
		Plan.ExitOrdinal, 1);

	const FGuidelineNode* ExitNode = Airport.Net->GetGuidelineNode(Plan.Exit);
	if (TestNotNull(TEXT("the chosen exit resolves to a guideline node"), ExitNode))
	{
		// At exit 1's ARC START since 2026-09-06: the builder splits the runway ExitLength
		// (the profile default, 6000) before the junction and that is where the taxi-in
		// leaves the centreline, so the earliest usable node is just short of the junction,
		// on the centreline, and never at exit 2's.
		const double Along = ExitNode->Position.X - TwoExitFirstExitAt(Airframe).X;
		TestTrue(FString::Printf(TEXT("and it sits at exit 1's JUNCTION (its arc start, %.0f uu short of it), ")
			TEXT("not exit 2's - the earlier one, despite its longer taxi to the stand"), -Along),
			Along <= 0.0 && Along >= -6000.0 - 1.0 && FMath::Abs(ExitNode->Position.Y) < 1.0);
	}

	return true;
}

// ---------------------------------------------------------------------------------------
// (d) VacateAt is the chosen exit's own projection onto the runway direction - measured
// from the SAME node the plan chose, not re-derived from the query point or the exit index.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerVacateAtTest,
	"Airside.Model.ArrivalPlanner.VacateAt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerVacateAtTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Airframe, { .ExitCount = 2 });

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Airport.Net, Airport.Threshold, Airframe);
	if (!TestTrue(TEXT("the arrival is accepted"), Plan.IsValid()))
	{
		return false;
	}

	const FGuidelineNode* ExitNode = Airport.Net->GetGuidelineNode(Plan.Exit);
	if (!TestNotNull(TEXT("the chosen exit resolves to a guideline node"), ExitNode))
	{
		return false;
	}

	const double Expected = FVector2D::DotProduct(ExitNode->Position - Plan.Threshold, Plan.Direction);
	TestEqual(TEXT("VacateAt is the chosen exit's own projection onto the runway direction"),
		Plan.VacateAt, Expected);

	return true;
}

// ---------------------------------------------------------------------------------------
// (e) NoExit: a runway long enough to stop on, with nothing on it to route from.
//
// RunwayExitNodes reads only the GUIDELINE graph, not the road graph - and the far end of
// ANY solved runway always sits exactly at Distance == Length from the threshold (Direction
// is defined as the unit vector toward it), with zero lateral offset, so it always qualifies
// as a candidate once RunwayLength >= Needed. That means a solved runway - even a bare one
// with no taxiway - always has at least one "exit": its own far end. So the only way to
// exercise NoExit is to ask NearestRunwayThreshold's answer (pure road-graph, needs no
// solve) against a network whose GUIDELINE graph was never derived at all: FRoadNetworkSolver
// ::SolveAll and FRoadGuidelineBuilder::Build are deliberately NOT called here, so
// GuidelineNodes stays empty and RunwayExitNodes has nothing to find - which is exactly
// "the runway can be stopped on, but nothing joins it far enough down to be usable."
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerNoExitTest,
	"Airside.Model.ArrivalPlanner.NoExit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerNoExitTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const double Needed = FLandingRun::RequiredLandingDistance(
		Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;

	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();

	// Long enough to stop on (RunwayLength > Needed), so the refusal cannot be RunwayTooShort.
	const FRoadNodeId A = Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Network->AddNode(FVector2D(Needed * 2.0, 0.0));
	Network->AddStraightSegment(A, B, Runway);

	// NOT solved and NOT built - see the test's own banner comment for why that is the whole
	// point rather than an oversight.

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Network, FVector2D::ZeroVector, Airframe);

	TestEqual(TEXT("a runway with no derived guideline graph refuses NoExit"),
		Plan.Why, EArrivalRefusal::NoExit);
	TestEqual(TEXT("and ExitCount is reported as zero"), Plan.ExitCount, 0);
	return true;
}

// ---------------------------------------------------------------------------------------
// (f) NoRouteToStand: an exit exists - the taxiway leaves the runway and goes somewhere -
// but nothing on the far end of it is a stand, so no route reaches one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerNoRouteToStandTest,
	"Airside.Model.ArrivalPlanner.NoRouteToStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerNoRouteToStandTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const double Needed = FLandingRun::RequiredLandingDistance(
		Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
	const double RunwayLength = Needed * 1.5;

	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	URoadProfile* Taxiway = TestProfiles::Taxiway();

	const FVector2D ThresholdAt(0.0, 0.0);
	const FVector2D ExitAt(RunwayLength * 0.8, 0.0);
	const FVector2D FarAt(RunwayLength, 0.0);

	// Split at the exit, same as ArrivalDispatchTest, so a guideline node lands on the runway
	// centreline for RunwayExitNodes to find.
	const FRoadNodeId Threshold = Network->AddNode(ThresholdAt);
	const FRoadNodeId Exit = Network->AddNode(ExitAt);
	const FRoadNodeId Far = Network->AddNode(FarAt);
	Network->AddStraightSegment(Threshold, Exit, Runway);
	Network->AddStraightSegment(Exit, Far, Runway);

	// The taxiway leaves the runway and goes somewhere - a dead end, nothing on it. NO
	// stand is placed anywhere in this network, which is the whole point: an exit exists
	// (this junction), but UEntityInstance::GetEntities() is empty, so the route search
	// inside ArrivalPlanner::Plan never finds a Goal to search for at all.
	const FRoadNodeId TaxiEnd = Network->AddNode(ExitAt + FVector2D(0.0, -20000.0));
	Network->AddStraightSegment(Exit, TaxiEnd, Taxiway);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Network);
	FRoadGuidelineBuilder::Build(*Network, Solved);

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Network, ThresholdAt, Airframe);

	TestEqual(TEXT("an exit with no reachable stand refuses NoRouteToStand"),
		Plan.Why, EArrivalRefusal::NoRouteToStand);
	TestTrue(TEXT("and at least one exit was found - the refusal is about the STAND, not the exit"),
		Plan.ExitCount > 0);
	return true;
}

// ---------------------------------------------------------------------------------------
// (g) NotAdmitted: the runway is there, long enough and free, but this aircraft may not
// use it. Refused BEFORE occupancy and exits, with the admission decision on the plan and
// the surface named in the sentence - the reason an aircraft is turned away must be the
// one the player can act on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerNotAdmittedTest,
	"Airside.Model.ArrivalPlanner.NotAdmitted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerNotAdmittedTest::RunTest(const FString& Parameters)
{
	FAirframe Airframe = TestAirframes::Piper();
	Airframe.Requirements = TestAirframes::PiperRequirements();
	const FTestAirport A = FTestAirport::Build(Airframe, { .ExitCount = 2 });

	FVector2D Threshold, Direction; double Length = 0.0; FRoadSegmentId Seed;
	if (!TestTrue(TEXT("the fixture has a runway"), A.Net->NearestRunwayThreshold(A.Threshold, Threshold, Direction, Length, &Seed))) { return false; }
	FRunwayFacts Grass;
	Grass.Surface = ERunwaySurface::Grass;
	TestTrue(TEXT("the strip becomes grass"), A.Net->SetRunwayFacts(Seed, Grass));

	const FArrivalPlan OnGrass = ArrivalPlanner::Plan(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	TestTrue(FString::Printf(TEXT("the Piper may land on grass: %s"), *ArrivalPlanner::DescribeRefusal(OnGrass)), OnGrass.IsValid());

	Airframe.Requirements.MinimumSurface = ERunwaySurface::Tarmac;
	const FArrivalPlan Refused = ArrivalPlanner::Plan(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	TestEqual(TEXT("an aircraft needing tarmac is refused the grass strip as NotAdmitted"), Refused.Why, EArrivalRefusal::NotAdmitted);
	TestEqual(TEXT("with the admission's own reason on the plan"), Refused.Admission.Why, ERunwayRefusal::Surface);
	TestEqual(TEXT("and the chain it was refused for"), Refused.RunwayChain.Num(), 3);
	const FString Sentence = ArrivalPlanner::DescribeRefusal(Refused);
	TestTrue(FString::Printf(TEXT("the sentence names the surface: %s"), *Sentence), Sentence.Contains(TEXT("grass")));
	return true;
}

#endif
