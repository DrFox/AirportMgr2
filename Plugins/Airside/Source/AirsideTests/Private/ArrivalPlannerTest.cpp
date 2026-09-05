#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/LandingRun.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	FAirframe MakePiperAirframe()
	{
		FAirframe Airframe;
		Airframe.Ground = UAircraftType::PiperMeridianGround();
		Airframe.Climb = UAircraftType::PiperMeridianClimb();
		Airframe.Approach = UAircraftType::PiperMeridianApproach();
		Airframe.Engine = UAircraftType::PiperMeridianEngine();
		return Airframe;
	}

	/**
	 * A runway with TWO exits, a taxiway off each, both eventually reaching ONE stand - but
	 * only the second exit's taxiway reaches it directly. The first exit's taxiway is joined
	 * to the second's by a crossbar, so a route exists from EITHER exit, and the one from the
	 * later exit is unambiguously shorter. That is the fixture the "earliest exit wins" rule
	 * needs: a network where picking by shortest taxi alone would give the wrong answer.
	 *
	 * Positions are handed back because the tests need Threshold, Direction and both exits'
	 * x-coordinates to state what they expect - re-deriving them from the network would risk
	 * testing the fixture against itself.
	 */
	struct FTwoExitAirport
	{
		URoadNetwork* Network = nullptr;
		FVector2D Threshold;
		FVector2D Direction;
		double RunwayLength = 0.0;
		double Needed = 0.0;
		FVector2D Exit1At;
		FVector2D Exit2At;
	};

	FTwoExitAirport BuildTwoExitAirport(UObject* Outer, const FAirframe& Airframe)
	{
		FTwoExitAirport Out;
		Out.Network = NewObject<URoadNetwork>(Outer);
		Out.Threshold = FVector2D(0.0, 0.0);
		Out.Direction = FVector2D(1.0, 0.0);

		Out.Needed = FLandingRun::RequiredLandingDistance(
			Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
		Out.RunwayLength = Out.Needed * 3.0;
		Out.Exit1At = FVector2D(Out.Needed * 1.2, 0.0);
		Out.Exit2At = FVector2D(Out.Needed * 2.0, 0.0);
		const FVector2D FarAt(Out.RunwayLength, 0.0);

		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

		// The runway, SPLIT at both exits - see ArrivalDispatchTest for why a T-junction is
		// what puts a guideline node on the centreline for RunwayExitNodes to find.
		const FRoadNodeId ThresholdNode = Out.Network->AddNode(Out.Threshold);
		const FRoadNodeId Exit1Node = Out.Network->AddNode(Out.Exit1At);
		const FRoadNodeId Exit2Node = Out.Network->AddNode(Out.Exit2At);
		const FRoadNodeId FarNode = Out.Network->AddNode(FarAt);
		Out.Network->AddStraightSegment(ThresholdNode, Exit1Node, Runway);
		Out.Network->AddStraightSegment(Exit1Node, Exit2Node, Runway);
		Out.Network->AddStraightSegment(Exit2Node, FarNode, Runway);

		// Exit 1's taxiway runs south to a dead end - no stand on it directly.
		const FRoadNodeId Taxi1End = Out.Network->AddNode(Out.Exit1At + FVector2D(0.0, -20000.0));
		Out.Network->AddStraightSegment(Exit1Node, Taxi1End, Taxiway);

		// Exit 2's taxiway runs south the same distance, and THIS is the one the stand sits
		// beside.
		const FRoadNodeId Taxi2End = Out.Network->AddNode(Out.Exit2At + FVector2D(0.0, -20000.0));
		Out.Network->AddStraightSegment(Exit2Node, Taxi2End, Taxiway);

		// The crossbar. Without it exit 1 could not reach the stand at all and this would
		// only prove "the planner takes the only route available" rather than "the planner
		// takes the EARLIEST exit despite a longer taxi".
		Out.Network->AddStraightSegment(Taxi1End, Taxi2End, Taxiway);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Out.Network);
		FRoadGuidelineBuilder::Build(*Out.Network, Solved);

		// THE STAND SITS BESIDE EXIT 2'S TAXIWAY AND FACES IT - see ArrivalDispatchTest's own
		// comment on FAnchorLink: the lead-in casts from the stand along heading + 180, so a
		// stand facing east (heading 0) casts its ray west and meets a taxiway to its west.
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		const FVector2D StandAt = Out.Exit2At + FVector2D(9000.0, -10000.0);
		Out.Network->PlaceEntity(Stand, Stand->Anchors, StandAt, 0.0);

		FAnchorLink::Build(*Out.Network);

		return Out;
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
	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Network, FVector2D::ZeroVector, MakePiperAirframe());

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
	const FAirframe Airframe = MakePiperAirframe();
	const double Needed = FLandingRun::RequiredLandingDistance(
		Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;

	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;

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
// marginally shorter one. See FTwoExitAirport for why BOTH exits can reach the one stand.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerEarliestExitWinsTest,
	"Airside.Model.ArrivalPlanner.EarliestExitWins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerEarliestExitWinsTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = MakePiperAirframe();
	const FTwoExitAirport Airport = BuildTwoExitAirport(GetTransientPackage(), Airframe);

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Airport.Network, Airport.Threshold, Airframe);

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

	const FGuidelineNode* ExitNode = Airport.Network->GetGuidelineNode(Plan.Exit);
	if (TestNotNull(TEXT("the chosen exit resolves to a guideline node"), ExitNode))
	{
		TestEqual(TEXT("and it sits at exit 1's JUNCTION, not exit 2's - the earlier one, ")
			TEXT("despite its longer taxi to the stand"),
			ExitNode->Position, Airport.Exit1At);
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
	const FAirframe Airframe = MakePiperAirframe();
	const FTwoExitAirport Airport = BuildTwoExitAirport(GetTransientPackage(), Airframe);

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Airport.Network, Airport.Threshold, Airframe);
	if (!TestTrue(TEXT("the arrival is accepted"), Plan.IsValid()))
	{
		return false;
	}

	const FGuidelineNode* ExitNode = Airport.Network->GetGuidelineNode(Plan.Exit);
	if (!TestNotNull(TEXT("the chosen exit resolves to a guideline node"), ExitNode))
	{
		return false;
	}

	const double Expected = FVector2D::DotProduct(ExitNode->Position - Plan.Threshold, Plan.Direction);
	TestEqual(TEXT("VacateAt is the chosen exit's own projection onto the runway direction"),
		Plan.VacateAt, Expected);

	return true;
}

#endif
