#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/TakeoffRun.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogDepartureTest, Log, All);

namespace
{
	/**
	 * A runway W -> E with one 45 degree taxiway joining at X, angled like a rapid exit for
	 * an EASTBOUND landing (it points east-south-east away from the runway), and a stand
	 * beside it. Entering the runway from it heading east is the hairpin; heading WEST is
	 * the natural join. Which threshold a departure uses is decided by the point on the
	 * runway it is asked for, so the same fixture serves the intersection case (depart
	 * westbound from the E threshold: the forward entry from this taxiway is toward W...)
	 * - see each test for the geometry it asks about.
	 */
	struct FDepartureAirport
	{
		URoadNetwork* Net = nullptr;
		FVector2D WAt = FVector2D(-40000.0, 0.0);
		FVector2D EAt = FVector2D(60000.0, 0.0);
		FVector2D XAt = FVector2D(20000.0, 0.0);
		FGuidelineNodeId StandNode;
	};

	FDepartureAirport BuildDepartureAirport(UObject* Outer)
	{
		FDepartureAirport Out;
		Out.Net = NewObject<URoadNetwork>(Outer);
		URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
		const FRoadNodeId W = Out.Net->AddNode(Out.WAt);
		const FRoadNodeId X = Out.Net->AddNode(Out.XAt);
		const FRoadNodeId E = Out.Net->AddNode(Out.EAt);
		const FRoadNodeId T = Out.Net->AddNode(Out.XAt + FVector2D(20000.0, -20000.0));
		Out.Net->AddStraightSegment(W, X, Runway);
		Out.Net->AddStraightSegment(X, E, Runway);
		Out.Net->AddStraightSegment(X, T, Taxiway);
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Out.Net);
		FRoadGuidelineBuilder::Build(*Out.Net, Solved);
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		const FEntityInstanceId StandId = Out.Net->PlaceEntity(Stand, Stand->Anchors, Out.XAt + FVector2D(25000.0, -14000.0), 0.0);
		FAnchorLink::Build(*Out.Net);
		for (const FEntityInstance& Instance : Out.Net->GetEntities())
		{
			if (Instance.bAlive && Instance.PoseNode.IsSet()) { Out.StandNode = Instance.PoseNode; }
		}
		return Out;
	}

	bool DepartureRouteUsesRunway(const URoadNetwork& Net, const FRoutePlan& Plan)
	{
		for (const FRouteStep& Step : Plan.Steps)
		{
			const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Step.Edge);
			if (Edge && Edge->DerivedFrom.IsSet() && Net.IsRunwaySegment(Edge->DerivedFrom)) { return true; }
		}
		return false;
	}
}

/**
 * AN INTERSECTION DEPARTURE. Asked to depart WESTBOUND (a point near the E threshold),
 * the taxiway's arc toward W is the forward entry: the planner picks the split node on
 * that side, the taxi never touches a runway edge, arrives heading west, and the roll is
 * judged on the 54000 uu from there to W - plenty for a Piper's ~23000.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeparturePlannerIntersectionTest,
	"Airside.Model.DeparturePlanner.Intersection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDeparturePlannerIntersectionTest::RunTest(const FString& Parameters)
{
	FDepartureAirport A = BuildDepartureAirport(GetTransientPackage());
	if (!TestTrue(TEXT("the stand is linked"), A.StandNode.IsSet())) { return false; }
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const double Needed = FTakeoffRun::RequiredRoll(Airframe.Ground, Airframe.Climb);

	const FDeparturePlan Plan = DeparturePlanner::Plan(*A.Net, A.StandNode, A.EAt - FVector2D(1000.0, 0.0), Airframe, ETraversalClass::Aircraft);
	UE_LOG(LogDepartureTest, Log, TEXT("%s"), *DeparturePlanner::Describe(Plan));
	if (!TestTrue(FString::Printf(TEXT("planned: %s"), *DeparturePlanner::Describe(Plan)), Plan.IsValid())) { return false; }

	TestTrue(TEXT("departing from the E threshold, westbound"), Plan.Direction.X < -0.99 && FVector2D::Distance(Plan.Threshold, A.EAt) < 1.0);
	TestFalse(TEXT("an intersection departure, not a backtrack"), Plan.bBacktrack);
	// The split node on the W side of X, ExitLength (6000) from it: 46000 from the E threshold.
	TestTrue(FString::Printf(TEXT("joins at the entry arc's node, 46000 uu past the threshold (%.0f)"), Plan.EntryOffset),
		FMath::Abs(Plan.EntryOffset - 46000.0) < 1.0);
	TestTrue(FString::Printf(TEXT("with %.0f uu ahead, at least the %.0f needed"), Plan.Available, Needed), Plan.Available >= Needed);
	TestFalse(TEXT("the taxi never runs along the runway"), DepartureRouteUsesRunway(*A.Net, Plan.Route));
	const int32 N = Plan.Route.Polyline.Num();
	TestTrue(TEXT("and arrives heading down the runway"),
		N >= 2 && FVector2D::DotProduct(Plan.Route.Polyline[N - 1] - Plan.Route.Polyline[N - 2], Plan.Direction) > 0.0);
	TestTrue(TEXT("ending on the centreline"), FMath::Abs(Plan.Route.Polyline.Last().Y) < 1.0);
	return true;
}

/**
 * A BACKTRACK when nothing else will do. Asked to depart EASTBOUND from W, the only forward
 * entry from this taxiway is the hairpin onto the E side of X, which leaves 34000 uu -
 * enough for the Piper, so make the runway short enough that it is not: then the planner
 * routes to the threshold with runway edges allowed and says so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeparturePlannerBacktrackTest,
	"Airside.Model.DeparturePlanner.Backtrack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDeparturePlannerBacktrackTest::RunTest(const FString& Parameters)
{
	// A runway only 30000 long past X: the hairpin entry at X + 6000 leaves 24000, short of
	// the Piper's roll with margin; the threshold at W leaves the whole 90000.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
	const FVector2D WAt(-60000.0, 0.0), XAt(0.0, 0.0), EAt(30000.0, 0.0);
	const FRoadNodeId W = Net->AddNode(WAt);
	const FRoadNodeId X = Net->AddNode(XAt);
	const FRoadNodeId E = Net->AddNode(EAt);
	const FRoadNodeId T = Net->AddNode(XAt + FVector2D(20000.0, -20000.0));
	Net->AddStraightSegment(W, X, Runway);
	Net->AddStraightSegment(X, E, Runway);
	Net->AddStraightSegment(X, T, Taxiway);
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	FRoadGuidelineBuilder::Build(*Net, Solved);
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	Net->PlaceEntity(Stand, Stand->Anchors, XAt + FVector2D(25000.0, -14000.0), 0.0);
	FAnchorLink::Build(*Net);
	FGuidelineNodeId StandNode;
	for (const FEntityInstance& Instance : Net->GetEntities()) { if (Instance.bAlive) { StandNode = Instance.PoseNode; } }
	if (!TestTrue(TEXT("the stand is linked"), StandNode.IsSet())) { return false; }

	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const double Needed = FTakeoffRun::RequiredRoll(Airframe.Ground, Airframe.Climb);
	TestTrue(FString::Printf(TEXT("fixture: the hairpin entry leaves 24000, short of the %.0f needed"), Needed), Needed > 24000.0);

	const FDeparturePlan Plan = DeparturePlanner::Plan(*Net, StandNode, WAt + FVector2D(1000.0, 0.0), Airframe, ETraversalClass::Aircraft);
	UE_LOG(LogDepartureTest, Log, TEXT("%s"), *DeparturePlanner::Describe(Plan));
	if (!TestTrue(FString::Printf(TEXT("planned: %s"), *DeparturePlanner::Describe(Plan)), Plan.IsValid())) { return false; }
	TestTrue(TEXT("departing from the W threshold, eastbound"), Plan.Direction.X > 0.99);
	TestTrue(TEXT("a backtrack"), Plan.bBacktrack);
	// The strip's end node sits the runway's half width (900) inside the threshold: a dead
	// end is cut back by that much, and the threshold is the road node beyond the cut.
	TestTrue(FString::Printf(TEXT("to the threshold's own end node (%.0f past the threshold, within its 900 half width)"), Plan.EntryOffset),
		Plan.EntryOffset <= 900.0 + 1.0);
	TestTrue(TEXT("along the runway, which a backtrack may"), DepartureRouteUsesRunway(*Net, Plan.Route));
	TestTrue(FString::Printf(TEXT("with the whole runway ahead but that cut (%.0f)"), Plan.Available), Plan.Available >= 90000.0 - 901.0);
	return true;
}

/**
 * THE HANDOVER FROM TAXI TO ROLL IS CONTINUOUS: an intersection departure dispatched on
 * the planner's route rolls from where its taxi ended, at the speed it arrived with, and
 * never jumps to the threshold. Measured tick by tick like the arrival's handover test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepartureFromIntersectionIsContinuousTest,
	"Airside.Model.Traffic.DepartureFromIntersectionIsContinuous",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepartureFromIntersectionIsContinuousTest::RunTest(const FString& Parameters)
{
	FDepartureAirport A = BuildDepartureAirport(GetTransientPackage());
	if (!TestTrue(TEXT("the stand is linked"), A.StandNode.IsSet())) { return false; }
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FDeparturePlan Plan = DeparturePlanner::Plan(*A.Net, A.StandNode, A.EAt - FVector2D(1000.0, 0.0), Airframe, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("planned"), Plan.IsValid() && !Plan.bBacktrack)) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchAgent(A.Net, Plan.Route, Airframe, ETraversalClass::Aircraft, 10.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	TestTrue(TEXT("armed as a departure with the entry offset"),
		Traffic->FindAgent(Id)->bDepartureArmed && FMath::Abs(Traffic->FindAgent(Id)->DepartureOrder.EntryOffset - Plan.EntryOffset) < 1.0);

	constexpr double Dt = 1.0 / 60.0;
	const double PositionStepAllowed = (Airframe.Ground.Takeoff.SpeedCap + 1.0) * Dt * 1.5 + 5.0;
	bool bSawDeparting = false, bAirborne = false;
	FVector2D PrevAt = FVector2D::ZeroVector;
	bool bHavePrev = false;
	double WorstStep = 0.0, WorstAt = 0.0;
	double HandoverJump = -1.0;
	EAgentPhase PrevPhase = EAgentPhase::Taxiing;
	int32 Ticks = 0;
	for (; Ticks < 300 * 60; ++Ticks)
	{
		Traffic->Advance(Dt, A.Net);
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		if (Agent == nullptr) { break; }
		const FAgentMotion& M = Agent->LastMotion;
		if (bHavePrev)
		{
			const double Step = FVector2D::Distance(M.Position, PrevAt);
			if (Step > WorstStep) { WorstStep = Step; WorstAt = Ticks * Dt; }
			if (PrevPhase == EAgentPhase::Taxiing && Agent->Phase == EAgentPhase::Departing) { HandoverJump = Step; }
		}
		bSawDeparting = bSawDeparting || Agent->Phase == EAgentPhase::Departing;
		if (Agent->Phase == EAgentPhase::Departing && M.Altitude > 100.0) { bAirborne = true; break; }
		PrevAt = M.Position;
		PrevPhase = Agent->Phase;
		bHavePrev = true;
	}
	UE_LOG(LogDepartureTest, Log, TEXT("Departure handover: worst position step %.1f uu at %.2f s (allowed %.1f), handover frame step %.1f, airborne %d after %d ticks"),
		WorstStep, WorstAt, PositionStepAllowed, HandoverJump, bAirborne, Ticks);
	TestTrue(TEXT("the departure rolled"), bSawDeparting);
	TestTrue(TEXT("and got airborne within five minutes"), bAirborne);
	TestTrue(FString::Printf(TEXT("no position step beyond one tick at Vr (worst %.1f uu at %.2f s, allowed %.1f)"), WorstStep, WorstAt, PositionStepAllowed),
		WorstStep <= PositionStepAllowed);
	TestTrue(FString::Printf(TEXT("the handover frame moved the aircraft, not the aircraft to the threshold (%.1f uu)"), HandoverJump),
		HandoverJump >= 0.0 && HandoverJump <= PositionStepAllowed);
	return true;
}

/**
 * NOT ADMITTED. The strip is reachable and long enough, but the aircraft needs tarmac and
 * the strip is grass: refused before any entry is searched for, with the reason on the
 * plan and the surface in the sentence.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeparturePlannerNotAdmittedTest,
	"Airside.Model.DeparturePlanner.NotAdmitted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDeparturePlannerNotAdmittedTest::RunTest(const FString& Parameters)
{
	FDepartureAirport A = BuildDepartureAirport(GetTransientPackage());
	if (!TestTrue(TEXT("the stand is linked"), A.StandNode.IsSet())) { return false; }
	FVector2D Threshold, Direction; double Length = 0.0; FRoadSegmentId Seed;
	if (!TestTrue(TEXT("the fixture has a runway"), A.Net->RunwayExtentAt(A.XAt, Threshold, Direction, Length, &Seed))) { return false; }
	FRunwayFacts Grass;
	Grass.Surface = ERunwaySurface::Grass;
	A.Net->SetRunwayFacts(Seed, Grass);

	FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FDeparturePlan OnGrass = DeparturePlanner::Plan(*A.Net, A.StandNode, A.EAt - FVector2D(1000.0, 0.0), Airframe, ETraversalClass::Aircraft);
	TestTrue(FString::Printf(TEXT("the Piper may depart from grass: %s"), *DeparturePlanner::Describe(OnGrass)), OnGrass.IsValid());

	Airframe.Requirements.MinimumSurface = ERunwaySurface::Tarmac;
	const FDeparturePlan Refused = DeparturePlanner::Plan(*A.Net, A.StandNode, A.EAt - FVector2D(1000.0, 0.0), Airframe, ETraversalClass::Aircraft);
	TestEqual(TEXT("an aircraft needing tarmac is refused the grass strip as NotAdmitted"), Refused.Why, EDepartureRefusal::NotAdmitted);
	TestEqual(TEXT("with the admission's own reason on the plan"), Refused.Admission.Why, ERunwayRefusal::Surface);
	TestFalse(TEXT("and no route was planned"), Refused.Route.IsValid());
	const FString Sentence = DeparturePlanner::Describe(Refused);
	TestTrue(FString::Printf(TEXT("the sentence names the surface: %s"), *Sentence), Sentence.Contains(TEXT("grass")));
	return true;
}

#endif
