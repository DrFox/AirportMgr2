#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/PushbackRun.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/**
	 * A runway, a stand, and a JUNCTION with two arms - which is the shape a pushback needs.
	 *
	 *   B (0, 0)          on the runway, where a departure is heading
	 *   J (0, -10000)     where the stand's lead-in meets the taxiway
	 *   A (0, -20000)     the stand
	 *   C (0, -40000)     further out, so an aeroplane can arrive at A from the SOUTH instead
	 *   E (20000, -10000) the FAR ARM of the junction - what a push reverses onto
	 *
	 * WITHOUT E THERE IS NO PUSHBACK AT ALL. PushbackPlanner::Plan reverses onto the arm the
	 * departure does not take, so a junction with only two edges leaves it nothing to choose
	 * and DepartAgent refuses - which is the ruling, not a gap. An earlier version of this
	 * fixture had no E and started failing with NoPushbackRoute the moment that became true.
	 *
	 * Taxi B -> A and the aeroplane parks facing south, with its way out behind it: a push.
	 * Taxi C -> A and it parks facing north, already pointing the way it will leave: no push.
	 * One graph, one departure, opposite answers - which is what the straight-out rule
	 * measures.
	 */
	struct FPushbackGraph
	{
		FGuidelineNodeId A;
		FGuidelineNodeId B;
		FGuidelineNodeId C;
		FGuidelineNodeId J;
		FGuidelineNodeId E;
		FGuidelineEdgeId AJ;
	};

	FPushbackGraph PushbackBuildGraph(URoadNetwork& Net)
	{
		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId RA = Net.AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RM = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RB = Net.AddNode(FVector2D(50000.0, 0.0));
		Net.AddStraightSegment(RA, RM, Runway);
		Net.AddStraightSegment(RM, RB, Runway);

		FPushbackGraph G;
		G.B = TestGraph::Node(Net, 0.0, 0.0);
		G.J = TestGraph::Node(Net, 0.0, -10000.0);
		G.A = TestGraph::Node(Net, 0.0, -20000.0);
		G.C = TestGraph::Node(Net, 0.0, -40000.0);
		G.E = TestGraph::Node(Net, 20000.0, -10000.0);

		// bDerived FALSE, as DepartAgentTest's own edges are: an authored line survives the
		// solve, and nothing here rebuilds the graph.
		TestGraph::FJoinOptions Options;
		Options.bDerived = false;
		G.AJ = TestGraph::Join(Net, G.A, G.J, Options);
		TestGraph::Join(Net, G.J, G.B, Options);
		TestGraph::Join(Net, G.J, G.E, Options);
		TestGraph::Join(Net, G.C, G.A, Options);
		return G;
	}

	/** Taxis one aircraft From -> To and returns its id once it has parked, or 0. */
	int32 PushbackParkFacing(UGroundTraffic& Traffic, URoadNetwork& Net,
		FGuidelineNodeId From, FGuidelineNodeId To)
	{
		// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
		const int32 Id = Traffic.DispatchAgent(&Net, TestGraph::Probe(Net, From, To, ETraversalClass::Aircraft),
			TestAirframes::Piper(), ETraversalClass::Aircraft, /*ShutdownPauseSeconds*/ 0.0);
		if (Id <= 0)
		{
			return 0;
		}

		// THE PHASE SAYS IT ARRIVED, not a distance somebody computed - the shape
		// DepartAgentTest uses for the same wait.
		for (int32 I = 0; I < 20000 && Traffic.FindAgent(Id)->Phase != EAgentPhase::Parked; ++I)
		{
			Traffic.Advance(1.0 / 30.0, &Net);
		}
		return Traffic.FindAgent(Id)->Phase == EAgentPhase::Parked ? Id : 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackClearanceTest,
	"Airside.Model.PushbackClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackClearanceTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);

	// Taxied in from the runway, so it parks facing SOUTH with its way out behind it.
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("an aircraft parks facing away from its way out"), Id > 0))
	{
		return false;
	}

	// SOMEBODY ELSE IS STANDING ON THE GROUND THE PUSH NEEDS - the FAR ARM, which is where a
	// push goes, and not the way the departure will taxi. A phantom holder is how
	// GroundTrafficTest plants a blocker, and it is the right tool here: what is under test is
	// the clearance arithmetic, not a second aircraft's behaviour.
	FTrafficClaim Sitting;
	Sitting.AgentId = 99;
	Sitting.Resource = FTrafficResource::OfNode(G.E);
	Sitting.bOccupied = true;
	FTrafficClaim Blocker;
	Traffic->OccupancyForTest().TryClaim(Sitting, Blocker);

	// A PUSH IS GRANTED WHOLE OR NOT AT ALL. A manoeuvring agent cannot replan - a stand's
	// lead-in is the only way off it - so a push that could be stopped half way would be a
	// phase able to block a taxiway indefinitely with nothing able to act on it.
	TestEqual(TEXT("a push whose ground is occupied is refused"),
		Traffic->DepartAgent(Id, *Net), EDepartureRefusal::PushbackBlocked);

	// AND NOTHING WAS HALF-STARTED. This is the assertion that would catch a DepartAgent which
	// armed the phase and then discovered the refusal: an aeroplane left Manoeuvring with a
	// blocked route would sit on its own lead-in for ever.
	TestEqual(TEXT("and the aeroplane is still parked, not half-pushed"),
		Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked);

	// THE REFUSAL CLEARS ITSELF, unlike NoRoute or NotAdmitted. That is the whole reason it is
	// a separate value: a caller that treated it as permanent would give up on an aeroplane
	// that would have left seconds later.
	Traffic->OccupancyForTest().ReleaseAll(99);

	TestEqual(TEXT("and once the ground frees, the same aeroplane is cleared"),
		Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None);
	TestEqual(TEXT("into the manoeuvre, NOT straight into a taxi"),
		Traffic->FindAgent(Id)->Phase, EAgentPhase::Manoeuvring);

	// A PUSHING AEROPLANE HOLDS ITS OWN LEAD-IN. FClaimPass::Run's first arm releases every
	// guideline claim for a phase that is not on a route; a manoeuvring agent that fell into
	// it would show the stand's own line free with an aeroplane on it, and something could be
	// cleared down the line it is being pushed along.
	//
	// MEASURED AS A HOLD rather than asserted as a phase check: a test that merely named this
	// contract would pass on an implementation that took the wrong arm and happened to claim
	// the ground some other way.
	Traffic->Advance(1.0 / 30.0, Net);
	TestTrue(TEXT("a pushing aeroplane holds the lead-in it is standing on"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfEdge(G.AJ), /*ExcludingAgent*/ 0));

	// AND IT GETS OFF THE STAND. The push is bounded, so this must terminate: a manoeuvre that
	// never ended is the failure HasArrived's clamp to Plan.Length exists to prevent.
	int32 Ticks = 0;
	for (; Ticks < 20000 && Traffic->FindAgent(Id) != nullptr
		&& Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring; ++Ticks)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	TestTrue(FString::Printf(TEXT("the push finishes (%d ticks)"), Ticks), Ticks < 20000);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackStraightOutTest,
	"Airside.Model.PushbackStraightOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackStraightOutTest::RunTest(const FString& Parameters)
{
	// AN AEROPLANE ALREADY POINTING THE WAY IT WILL LEAVE NEEDS NO PUSH, and the rule is a
	// MEASUREMENT of the ground ahead rather than a flag on the stand - which is why one
	// question answers a taxi-through stand, a taxiway a player happened to draw past a stand,
	// and a graph rebuilt since the aeroplane parked.
	//
	// THE SAME GRAPH AS THE CLEARANCE TEST, driven the other way round. That is what makes
	// this a contrast rather than a second unrelated fixture: nothing about the airport
	// differs, only which direction the aeroplane arrived from.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);

	// Taxied in from the SOUTH, so it parks at A facing north - already pointing at the
	// runway it is about to depart from.
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.C, G.A);
	if (!TestTrue(TEXT("an aircraft parks facing its way out"), Id > 0))
	{
		return false;
	}

	const EDepartureRefusal Why = Traffic->DepartAgent(Id, *Net);
	if (!TestEqual(FString::Printf(TEXT("it is cleared to leave (%d)"), static_cast<int32>(Why)),
		Why, EDepartureRefusal::None))
	{
		return false;
	}

	TestEqual(TEXT("and drives straight out - it never enters the manoeuvre at all"),
		Traffic->FindAgent(Id)->Phase, EAgentPhase::Taxiing);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackDepartReleasesStandTest,
	"Airside.Model.PushbackDepartReleasesStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackDepartReleasesStandTest::RunTest(const FString& Parameters)
{
	// ISSUE #295: DepartAgent's push branch used to re-spell TakeGoal's three calls
	// (SetGoalFrom/ArmDepartureIfRunway/ClaimGoalNodeAtDispatch) by hand and never called
	// ReleaseGoal first - the one thing RedirectAgent and ExtendRoute already do before
	// TakeGoal. The stand the aeroplane is LEAVING stayed claimed against it for the rest of
	// the session, so a waiter re-offered stands (ReofferStands) could never be sent there
	// even after the aeroplane had climbed away and gone.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);

	// Taxied in from the runway, so it parks facing SOUTH with its way out behind it - a push
	// is needed, which is the branch DepartAgent's goal-change duplicate lived in.
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("an aircraft parks facing away from its way out"), Id > 0))
	{
		return false;
	}
	if (!TestEqual(TEXT("the stand is held by the parked aircraft"),
		Traffic->HolderOfNode(G.A), Id))
	{
		return false;
	}

	const EDepartureRefusal Why = Traffic->DepartAgent(Id, *Net);
	if (!TestEqual(FString::Printf(TEXT("the push is cleared (%d)"), static_cast<int32>(Why)),
		Why, EDepartureRefusal::None))
	{
		return false;
	}
	TestEqual(TEXT("into the manoeuvre"), Traffic->FindAgent(Id)->Phase, EAgentPhase::Manoeuvring);

	// THE OLD STAND IS FREE THE INSTANT THE PUSH IS GRANTED - between ticks, the same window
	// ReleaseGoal documents for RedirectAgent/ExtendRoute, not something a re-offer pass has
	// to wait a frame to discover.
	TestEqual(TEXT("the stand it just left is no longer held by anyone"),
		Traffic->HolderOfNode(G.A), 0);

	return true;
}

/**
 * A REBUILD DURING A PUSH RE-RESOLVES THE TAXI OUT TOO, not only the push.
 *
 * REPORTED FROM PLAY, 2026-09-27: the player dragged a runway's end junction while an SR22 was
 * pushing back. The release rebuild re-resolved the PUSH route ("1 agents re-resolved, 0
 * replanned") - but FRoadAgent::TaxiOutPlan, the route the follower takes over when the push
 * ends, was never looked at, and the aeroplane taxied out along lines that no longer existed.
 * TaxiOutPlan is TaxiInPlan's mirror (its own comment says so), and TaxiInPlan IS re-resolved
 * for an arriving aircraft; this is the same treatment for the other one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackRebuildReResolvesTaxiOutTest,
	"Airside.Model.PushbackRebuildReResolvesTaxiOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackRebuildReResolvesTaxiOutTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);

	// Parked facing south from B -> A, so leaving means a push onto E, then E -> J -> B.
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("parked"), Id > 0)) { return false; }
	if (!TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)
		|| !TestEqual(TEXT("into the manoeuvre"), Traffic->FindAgent(Id)->Phase, EAgentPhase::Manoeuvring))
	{
		return false;
	}

	// MID-PUSH, THE TAXI-OUT'S LAST LINE IS REDRAWN: J -> B removed, a bypass J -> K -> B laid.
	FGuidelineEdgeId JB;
	for (int32 Index = 0; Index < Net->GetGuidelineEdges().Num(); ++Index)
	{
		const FGuidelineEdge& Edge = Net->GetGuidelineEdges()[Index];
		if (Edge.bAlive && ((Edge.A == G.J && Edge.B == G.B) || (Edge.A == G.B && Edge.B == G.J)))
		{
			JB.Index = Index;
			JB.Generation = Edge.Generation;
		}
	}
	if (!TestTrue(TEXT("the J-B line exists to redraw"), JB.IsSet())) { return false; }
	Net->RemoveGuidelineEdge(JB);
	const FGuidelineNodeId K = TestGraph::Node(*Net, -8000.0, -5000.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Net, G.J, K, Options);
	TestGraph::Join(*Net, K, G.B, Options);
	Traffic->OnGraphRebuilt(*Net);

	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("still there"), Agent)) { return false; }
	TestEqual(TEXT("still pushing - the push itself was untouched"), Agent->Phase, EAgentPhase::Manoeuvring);
	int32 Dead = 0;
	for (const FRouteStep& Step : Agent->TaxiOutPlan.Steps)
	{
		Dead += Net->GetGuidelineEdge(Step.Edge) == nullptr ? 1 : 0;
	}
	TestEqual(TEXT("the taxi out names no dead line"), Dead, 0);
	const bool bViaK = Agent->TaxiOutPlan.Polyline.ContainsByPredicate([](const FVector2D& P)
	{
		return FVector2D::Distance(P, FVector2D(-8000.0, -5000.0)) < 100.0;
	});
	TestTrue(TEXT("and it goes by the bypass the player drew"), bViaK);
	return true;
}

/**
 * A PUSH WHOSE WAY OUT WAS REDRAWN FROM UNDER IT DRIVES OFF - it does not teleport.
 *
 * REPORTED FROM PLAY, 2026-09-27, after the fix above: a second drag moved the node the push
 * and its taxi-out both start from. Neither re-resolved (the match radius is 25 uu), both were
 * STRANDED, the departure was re-armed from the dead taxi-out's end, and when the push finished
 * the follower was started on that dead route - the aeroplane jumped to where the old runway
 * began, facing the wrong way, crabbed round and took off. A stranded route an aircraft has not
 * started is not a place to stop; it is a route to plan again, from wherever the push ends.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackStrandedTaxiOutReplansTest,
	"Airside.Model.PushbackStrandedTaxiOutReplans",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackStrandedTaxiOutReplansTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("parked"), Id > 0)) { return false; }
	if (!TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None))
	{
		return false;
	}

	// PAST THE JUNCTION FIRST: in play the push was on its second step (J -> E) when the edit
	// landed, so the node its CURRENT step starts from moved - which is what strands a route.
	// A first version moved J while the push was still on A -> J; both routes then re-planned
	// cleanly and the test passed on the code that teleported (2026-09-27).
	for (int32 Tick = 0; Tick < 30 * 120 && Traffic->FindAgent(Id)->Pushback.Travelled < 12000.0; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("the push is past the junction, on its way to E"),
		Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring && Traffic->FindAgent(Id)->Pushback.Travelled >= 12000.0))
	{
		return false;
	}

	// THE JUNCTION AND THE FAR ARM MOVE 5 m - far past the 25 uu re-resolve radius. J is where
	// the push's current step starts; E is where the push ends and the taxi-out starts. Neither
	// route can be matched by position, and both strand, exactly as in play.
	Net->RemoveGuidelineNode(G.J);
	Net->RemoveGuidelineNode(G.E);
	const FGuidelineNodeId J2 = TestGraph::Node(*Net, 0.0, -10500.0);
	const FGuidelineNodeId E2 = TestGraph::Node(*Net, 20000.0, -10500.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Net, G.A, J2, Options);
	TestGraph::Join(*Net, J2, G.B, Options);
	TestGraph::Join(*Net, J2, E2, Options);
	Traffic->OnGraphRebuilt(*Net);
	const FGraphRebuildSummary Summary = Traffic->GetLastRebuildSummaryForTest();
	TestTrue(FString::Printf(TEXT("the rebuild's counts are never negative (%d re-resolved)"), Summary.ReResolved),
		Summary.ReResolved >= 0);

	// DRIVE IT OUT, watching every frame for a jump. At taxi speed a thirtieth of a second is
	// tens of uu; a teleport to the old runway start is thousands.
	FVector2D Last = Traffic->FindAgent(Id)->LastMotion.Position;
	double WorstJump = 0.0;
	bool bDeparted = false;
	int32 DeadAtHandover = INDEX_NONE;   // dead lines in the route it taxis on, read once
	for (int32 Tick = 0; Tick < 30 * 300 && Traffic->FindAgent(Id) != nullptr; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		const FRoadAgent* Now = Traffic->FindAgent(Id);
		if (Now == nullptr) { break; }
		// MEASURED BEFORE THE DEPARTING CHECK: in play the jump happened ON the frame the roll
		// began - a dead taxi-out ended at once and the take-off started from its armed entry.
		// A first version broke out first and never saw it.
		WorstJump = FMath::Max(WorstJump, FVector2D::Distance(Last, Now->LastMotion.Position));
		Last = Now->LastMotion.Position;
		// THE ROUTE IT IS HANDED WHEN THE PUSH ENDS. The code before this fix drove the stranded
		// taxi-out's old polyline - every metre of it on lines the edit had removed.
		if (DeadAtHandover == INDEX_NONE && Now->Phase == EAgentPhase::Taxiing)
		{
			DeadAtHandover = 0;
			for (const FRouteStep& Step : Now->Follower.Plan.Steps)
			{
				DeadAtHandover += Net->GetGuidelineEdge(Step.Edge) == nullptr ? 1 : 0;
			}
		}
		if (Now->Phase == EAgentPhase::Departing) { bDeparted = true; break; }
	}
	TestEqual(TEXT("it taxis out on live lines only - none the edit removed"), DeadAtHandover, 0);
	TestTrue(FString::Printf(TEXT("it never jumps: worst frame-to-frame move %.0f uu"), WorstJump), WorstJump < 200.0);
	TestTrue(TEXT("and it reaches the runway and rolls"), bDeparted);
	return true;
}

#endif
