#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogM2HeadOnTest, Log, All);

namespace
{
	// Prefixed against the unity build: GroundTrafficTest.cpp and HoldShortMarkTest.cpp
	// own the unprefixed names.

	/** The guideline node derived for one end of Segment, or unset. */
	FGuidelineNodeId M2HeadOnNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].Origin.Segment == Segment && Nodes[Index].Origin.bEndA == bEndA)
			{
				return Net.GuidelineNodeIdAt(Index);
			}
		}
		return FGuidelineNodeId();
	}

	FAirframe M2HeadOnPiper()
	{
		FAirframe A;
		A.Ground = UAircraftType::PiperMeridianGround();
		A.Climb = UAircraftType::PiperMeridianClimb();
		A.Approach = UAircraftType::PiperMeridianApproach();
		A.Engine = UAircraftType::PiperMeridianEngine();
		return A;
	}

	FRoutePlan M2HeadOnRoute(const URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B)
	{
		FRouteQuery Q; Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
		return RouteSearch::Find(Net, Q);
	}

	bool M2HeadOnUsesRunway(const URoadNetwork& Net, const FRoutePlan& Plan)
	{
		for (const FRouteStep& Step : Plan.Steps)
		{
			const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Step.Edge);
			if (Edge != nullptr && Edge->DerivedFrom.IsSet() && Net.IsRunwaySegment(Edge->DerivedFrom))
			{
				return true;
			}
		}
		return false;
	}
}

/**
 * THE PIE DEADLOCK OF 2026-09-06 (samples/deadlock.png), rebuilt on the real solver and
 * guideline builder so the junctions, turn paths and runway exits are the genuine ones.
 *
 *   N1 ------------- N2                 top taxiway
 *    |                |
 *   [H2]              |
 *    W ======= X ======= E              runway, two segments, W and E thresholds
 *   [H]               |
 *    |                |
 *   S1 ------------- B ------- G        bottom taxiway; G is the arrival's stand-side goal
 *
 * Two departures taxi up the west taxiway to hold at bar H for the runway; an arrival is
 * vacating along the runway toward W with a taxi-in that turns at H and runs down the same
 * west taxiway - head on. In play the resolver replanned the arrival with only one EDGE
 * banned, so it looped W -> H2 -> back into H from the other arm, re-reserved the runway
 * along the way, and the two sat nose to nose for ever: "no member can turn". The route
 * the player could see - top taxiway, cross at X, bottom taxiway - was legal the whole
 * time and lost only on distance to a route through a node nobody could enter.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficHeadOnReplansRoundBarHolderTest,
	"Airside.Model.Traffic.HeadOnReplansRoundBarHolder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficHeadOnReplansRoundBarHolderTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	// 1800 wide, as the player's was (log: "Runway 09/27 placed, 50008 uu long, 1800 uu
	// wide"). The width sets where the taxiway's cut line - and so the bar - sits, and with
	// it whether the exit-to-bar turn path is a BOX (shorter than Footprint + Gap): on a
	// box the vacating aircraft stops at the exit node, which is what makes it a replan
	// candidate. On a 4500 runway the bar is 3750 out, the turn path is not a box, and the
	// aircraft stops mid-edge where nobody can turn - a different, later story.
	URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
	Runway->bContinuousThroughJunctions = true;
	// THE GEOMETRY THE DEADLOCK HAPPENED ON. With exit arcs (ExitLength > 0, the default
	// since 2026-09-06) the bar at H sits 60 m down the taxiway, an aircraft refused there
	// has its tail clear of the strip, the geometric release lets the runway go and no
	// cycle forms at all - the arcs doing their job (the whole-run measurement is
	// Airside.Model.Traffic.VacatedHandoverIsContinuous). The RESOLVER still has to break
	// the cycle when a bar IS within a fuselage of the asphalt - a stub taxiway, a profile
	// authored with a short exit, a hand-placed bar - so this replay keeps the straight
	// stubs it was recorded on by turning the arcs off for this runway.
	Runway->ExitLength = 0.0;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	const FRoadNodeId W = Net->AddNode(FVector2D(-40000.0, 0.0));
	const FRoadNodeId X = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(40000.0, 0.0));
	const FRoadSegmentId RW1 = Net->AddStraightSegment(W, X, Runway);
	const FRoadSegmentId RW2 = Net->AddStraightSegment(X, E, Runway);

	// The stand-side goal G sits on the bottom taxiway NEAR the west end, as the player's
	// stands did: from W the west taxiway is 45 km to it, the runway-and-crossing way
	// 85 km, so the arrival's fixed route is the head-on one. (A first cut put G east of
	// the crossing, and the search preferred rolling back along the runway - the probe
	// caught it before the assertion did.)
	const FRoadNodeId S1 = Net->AddNode(FVector2D(-40000.0, -25000.0));
	const FRoadNodeId G = Net->AddNode(FVector2D(-20000.0, -25000.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(0.0, -25000.0));
	const FRoadSegmentId T1 = Net->AddStraightSegment(W, S1, Taxiway);     // west taxiway, the blue line
	const FRoadSegmentId S1G = Net->AddStraightSegment(S1, G, Taxiway);    // bottom taxiway, west half
	Net->AddStraightSegment(G, B, Taxiway);                                // bottom taxiway, east half
	const FRoadSegmentId XB = Net->AddStraightSegment(X, B, Taxiway);      // the crossing's south arm

	const FRoadNodeId N1 = Net->AddNode(FVector2D(-40000.0, 20000.0));
	const FRoadNodeId N2 = Net->AddNode(FVector2D(0.0, 20000.0));
	const FRoadSegmentId Top1 = Net->AddStraightSegment(W, N1, Taxiway);   // the loop's north arm
	Net->AddStraightSegment(N1, N2, Taxiway);                              // top taxiway
	const FRoadSegmentId N2X = Net->AddStraightSegment(N2, X, Taxiway);    // the crossing's north arm

	{
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		FRoadGuidelineBuilder::Build(*Net, Solved);
	}

	// The bars the player placed: both arms at W, and both approaches to the crossing at X.
	const FGuidelineNodeId H = M2HeadOnNodeFor(*Net, T1, /*bEndA=*/true);
	const FGuidelineNodeId H2 = M2HeadOnNodeFor(*Net, Top1, /*bEndA=*/true);
	const FGuidelineNodeId Hn = M2HeadOnNodeFor(*Net, N2X, /*bEndA=*/false);
	const FGuidelineNodeId Hs = M2HeadOnNodeFor(*Net, XB, /*bEndA=*/true);
	if (!TestTrue(TEXT("the four bar nodes exist"), H.IsSet() && H2.IsSet() && Hn.IsSet() && Hs.IsSet())) { return false; }
	TestTrue(TEXT("bar at H"), Net->SetHoldShort(H, RW1));
	TestTrue(TEXT("bar at H2"), Net->SetHoldShort(H2, RW1));
	TestTrue(TEXT("bar north of the crossing"), Net->SetHoldShort(Hn, RW2));
	TestTrue(TEXT("bar south of the crossing"), Net->SetHoldShort(Hs, RW2));

	const FGuidelineNodeId RunwayW = M2HeadOnNodeFor(*Net, RW1, true);     // the threshold's own node
	const FGuidelineNodeId RunwayX = M2HeadOnNodeFor(*Net, RW1, false);    // RW1's node at the crossing
	const FGuidelineNodeId Bottom = M2HeadOnNodeFor(*Net, T1, false);      // T1's S1 end
	const FGuidelineNodeId Goal = M2HeadOnNodeFor(*Net, S1G, false);       // S1->G's G end, the stand side
	if (!TestTrue(TEXT("the route endpoints exist"), RunwayW.IsSet() && RunwayX.IsSet() && Bottom.IsSet() && Goal.IsSet())) { return false; }

	// THE ARRIVAL'S FIXED ROUTE: along the runway to W, then the west taxiway - built the way
	// ArrivalPlanner would (runway to the exit, shortest taxi from there), spliced so the
	// runway steps carry DerivedFrom and hold the strip while it rolls.
	const FRoutePlan Rollout = M2HeadOnRoute(*Net, RunwayX, RunwayW);
	const FRoutePlan TaxiIn = M2HeadOnRoute(*Net, RunwayW, Goal);
	if (!TestTrue(TEXT("rollout and taxi-in routes exist"), Rollout.IsValid() && TaxiIn.IsValid())) { return false; }
	TestTrue(TEXT("the rollout is on the runway"), M2HeadOnUsesRunway(*Net, Rollout));
	{
		// PROBE: where the taxi-in actually goes, against where the bar nodes are.
		const FVector2D HAt = Net->GetGuidelineNode(H)->Position;
		const FVector2D H2At = Net->GetGuidelineNode(H2)->Position;
		FString Steps;
		for (const FRouteStep& Step : TaxiIn.Steps)
		{
			const FGuidelineNode* To = Net->GetGuidelineNode(Step.To);
			const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Step.Edge);
			Steps += FString::Printf(TEXT(" -> node %d (%.0f, %.0f)%s"), Step.To.Index,
				To ? To->Position.X : 0.0, To ? To->Position.Y : 0.0,
				(Edge && Edge->DerivedFrom.IsSet()) ? *FString::Printf(TEXT(" seg %d"), Edge->DerivedFrom.Index) : TEXT(""));
		}
		UE_LOG(LogM2HeadOnTest, Log, TEXT("PROBE H = node %d (%.0f, %.0f); H2 = node %d (%.0f, %.0f); RunwayW = node %d; taxi-in %.0f uu:%s"),
			H.Index, HAt.X, HAt.Y, H2.Index, H2At.X, H2At.Y, RunwayW.Index, TaxiIn.Length, *Steps);
	}
	TestTrue(TEXT("the taxi-in's first turn is INTO the bar node H - the head-on route"),
		TaxiIn.Steps.Num() > 0 && TaxiIn.Steps[0].To == H);
	const FRoutePlan ArrivalPlan = RouteSearch::Splice(Rollout, Rollout.Steps.Num(), TaxiIn);
	if (!TestTrue(TEXT("spliced"), ArrivalPlan.IsValid())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Arrival = Traffic->DispatchAgent(Net, ArrivalPlan, M2HeadOnPiper(), ETraversalClass::Aircraft, 1.0);
	const int32 Dep1 = Traffic->DispatchAgent(Net, M2HeadOnRoute(*Net, Bottom, RunwayW), M2HeadOnPiper(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("both dispatched"), Arrival > 0 && Dep1 > 0)) { return false; }

	// THE ARRIVAL HOLDS THE STRIP BY ITS BODY, as one that has just vacated does (spec §3.1,
	// route four): the Vacated handover arms CrossingRunway and the geometric release lets
	// go only once the tail is clear. A plain taxi holds the strip only through its runway
	// EDGE step and lets go the moment the step changes - which is why a first cut of this
	// fixture produced no deadlock at all: the departure was through before the arrival
	// reached the bar. In play the arrival was refused the bar node with its tail still on
	// the asphalt, and that is the state under test.
	TestTrue(TEXT("the arrival is staged as just-vacated"), Traffic->BeginCrossingForTest(Arrival, RW1));
	TestTrue(TEXT("the departure's route ends on the runway, so it is armed"), Traffic->FindAgent(Dep1)->bDepartureArmed);

	// Run: the departure reaches the bar first and holds (the arrival is on the strip);
	// the arrival reaches W and is refused H; the cycle forms; the resolver must send the
	// ARRIVAL the long way round - top taxiway, hold at the crossing's north bar while the
	// departure rolls, cross, bottom taxiway - and everybody finishes.
	bool bDepHeldAtBar = false;
	bool bArrivalRefusedH = false;
	double ArrivalMaxY = -1.0e9;
	bool bArrivalTaxiedOnRunwayAfterReplan = false;
	int32 ReplanTick = -1;
	bool bDepGone = false;
	int32 Ticks = 0;
	for (double Clock = 0.0; Clock < 400.0; Clock += 0.05, ++Ticks)
	{
		Traffic->Advance(0.05, Net);
		const FRoadAgent* A = Traffic->FindAgent(Arrival);
		const FRoadAgent* D = Traffic->FindAgent(Dep1);
		if (D == nullptr) { bDepGone = true; }
		if (A == nullptr) { break; }
		if (D != nullptr && D->WaitingOn == Arrival && D->Follower.Speed < 1e-6) { bDepHeldAtBar = true; }
		if (A->WaitingOn == Dep1 && A->BlockedResource.Kind == ETrafficResourceKind::Node && A->BlockedResource.Node == H)
		{
			bArrivalRefusedH = true;
		}
		if (ReplanTick < 0 && Traffic->GetLastResolvedAgentForTest() == Arrival)
		{
			ReplanTick = Ticks;
			// The replanned TAIL - the steps the agent has not yet ENTERED - must never taxi
			// along the strip again. The step it is standing on is the runway it arrived on
			// (it was refused with its tail still on the asphalt), and stays.
			double StepStart = 0.0;
			for (const FRouteStep& Step : A->Follower.Plan.Steps)
			{
				const bool bEntered = StepStart <= A->Follower.Travelled;
				StepStart = Step.EndDistance;
				if (bEntered) { continue; }
				const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Step.Edge);
				if (Edge != nullptr && Edge->DerivedFrom.IsSet() && Net->IsRunwaySegment(Edge->DerivedFrom))
				{
					bArrivalTaxiedOnRunwayAfterReplan = true;
				}
			}
		}
		if (ReplanTick >= 0) { ArrivalMaxY = FMath::Max(ArrivalMaxY, A->LastMotion.Position.Y); }
		if (bDepGone && A->Phase == EAgentPhase::Parked) { break; }
	}

	const FRoadAgent* A = Traffic->FindAgent(Arrival);
	UE_LOG(LogM2HeadOnTest, Log,
		TEXT("HeadOnReplansRoundBarHolder measured: departure held at bar %d, arrival refused H %d, ")
		TEXT("replan at tick %d, arrival max Y after replan %.0f, departure gone %d, arrival phase %d, ")
		TEXT("cycles %d, deadlock lines %d"),
		bDepHeldAtBar, bArrivalRefusedH, ReplanTick, ArrivalMaxY, bDepGone,
		A ? static_cast<int32>(A->Phase) : -1,
		Traffic->GetCyclesDetectedForTest(), Traffic->GetDeadlockLogLinesForTest());

	TestTrue(TEXT("the departure held at the bar while the arrival was on the strip"), bDepHeldAtBar);
	TestTrue(TEXT("the arrival was refused node H, the bar the departure stands on"), bArrivalRefusedH);
	TestEqual(TEXT("one cycle, detected once"), Traffic->GetCyclesDetectedForTest(), 1);
	TestEqual(TEXT("resolved by the ARRIVAL replanning - the bar-holder has one way out and the arrival has two"),
		Traffic->GetLastResolvedAgentForTest(), Arrival);
	TestTrue(TEXT("the replanned route goes round the top: the arrival passes the top taxiway (Y > 15000)"),
		ArrivalMaxY > 15000.0);
	TestFalse(TEXT("the replanned tail never taxis along the runway again"), bArrivalTaxiedOnRunwayAfterReplan);
	TestTrue(TEXT("the departure got its runway and departed"), bDepGone);
	TestTrue(TEXT("the arrival reached its goal"), A != nullptr && A->Phase == EAgentPhase::Parked);
	TestEqual(TEXT("one deadlock line: resolved, never 'no member can turn'"), Traffic->GetDeadlockLogLinesForTest(), 1);
	return true;
}

#endif
