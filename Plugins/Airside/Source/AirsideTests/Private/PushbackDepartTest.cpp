#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
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

	TArray<FAgentTransition> Heard;
	Traffic->OnAgentPhaseChanged.AddLambda([&Heard](const FAgentTransition& T) { Heard.Add(T); });
	TestEqual(TEXT("and once the ground frees, the same aeroplane is cleared"),
		Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None);
	TestEqual(TEXT("into the manoeuvre, NOT straight into a taxi"),
		Traffic->FindAgent(Id)->Phase, EAgentPhase::Manoeuvring);
	// THE PUSH'S CAUSE (#436), the same DepartOrdered the straight-out branch gives: both are the taxi out.
	TestTrue(TEXT("announced as DepartOrdered, Parked -> Manoeuvring"), Heard.Num() == 1
		&& Heard[0].Cause == EAgentEvent::DepartOrdered && Heard[0].To == EAgentPhase::Manoeuvring);

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

	TArray<FAgentTransition> Heard;
	Traffic->OnAgentPhaseChanged.AddLambda([&Heard](const FAgentTransition& T) { Heard.Add(T); });
	const EDepartureRefusal Why = Traffic->DepartAgent(Id, *Net);
	if (!TestEqual(FString::Printf(TEXT("it is cleared to leave (%d)"), static_cast<int32>(Why)),
		Why, EDepartureRefusal::None))
	{
		return false;
	}

	TestEqual(TEXT("and drives straight out - it never enters the manoeuvre at all"),
		Traffic->FindAgent(Id)->Phase, EAgentPhase::Taxiing);
	// THE TAXI OUT BY ITS CAUSE (#436): the straight-out branch is a RedirectAgent, and it must say DepartOrdered - the
	// flight board reads that as the taxi OUT where the plain redirect a re-offer makes stays the taxi in.
	TestTrue(TEXT("and announces it as DepartOrdered, Parked -> Taxiing"), Heard.Num() == 1
		&& Heard[0].Cause == EAgentEvent::DepartOrdered && Heard[0].From == EAgentPhase::Parked && Heard[0].To == EAgentPhase::Taxiing);

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
	// the session, so a waiter re-offered stands (RetryWaiters) could never be sent there
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
	// AND EVERY FRAME'S TURN (#501 re-review): the play report was "facing the wrong way, crabbed round" as well as the
	// jump, and a push has no steer law - a kink in the line it walks is an instant yaw. The rejoin's join leg meets the
	// moved arm at 14.04 degrees; nothing on this run may turn the body further in one frame.
	double LastHeading = Traffic->FindAgent(Id)->LastMotion.Heading;
	double WorstTurn = 0.0;
	bool bDeparted = false;
	int32 DeadAtHandover = INDEX_NONE;   // dead lines in the route it taxis on, read once
	// AND ONE SECOND INTO THE ROLL: the take-off places the aeroplane on its entry on the first
	// frame it rolls - see TaxiingDepartureStrandedDoesNotJump, which found this test stopped a
	// frame too early to see the jump.
	int32 RollFrames = 0;
	for (int32 Tick = 0; Tick < 30 * 300 && Traffic->FindAgent(Id) != nullptr && RollFrames < 30; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		const FRoadAgent* Now = Traffic->FindAgent(Id);
		if (Now == nullptr) { break; }
		// MEASURED BEFORE THE DEPARTING CHECK: in play the jump happened ON the frame the roll
		// began - a dead taxi-out ended at once and the take-off started from its armed entry.
		// A first version broke out first and never saw it.
		WorstJump = FMath::Max(WorstJump, FVector2D::Distance(Last, Now->LastMotion.Position));
		Last = Now->LastMotion.Position;
		WorstTurn = FMath::Max(WorstTurn, FMath::RadiansToDegrees(FMath::Abs(FMath::UnwindRadians(Now->LastMotion.Heading - LastHeading))));
		LastHeading = Now->LastMotion.Heading;
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
		if (Now->Phase == EAgentPhase::Departing) { bDeparted = true; ++RollFrames; }
	}
	TestEqual(TEXT("it taxis out on live lines only - none the edit removed"), DeadAtHandover, 0);
	TestTrue(FString::Printf(TEXT("it never jumps: worst frame-to-frame move %.0f uu"), WorstJump), WorstJump < 200.0);
	TestTrue(FString::Printf(TEXT("it never swings round: worst frame-to-frame turn %.2f deg"), WorstTurn), WorstTurn <= 14.1);
	TestTrue(TEXT("and it reaches the runway and rolls"), bDeparted);
	return true;
}

/**
 * A TAXIING DEPARTURE STRANDED BY AN EDIT DOES NOT JUMP TO THE RUNWAY.
 *
 * REPORTED FROM PLAY, 2026-09-27, after both fixes above: the re-planned taxi-out was being
 * driven when a third drag moved the node its current step starts from. The route stranded -
 * "stop where you are" - but a stranded route counts as ARRIVED, and the taxi-complete
 * handover then started the armed take-off at the armed entry, wherever the aeroplane was:
 * "Taxi complete; rolling for departure" on the rebuild's own frame, and a jump to the runway.
 * The take-off now starts only AT its entry; anywhere else the aeroplane holds, and a new way
 * to the runway is planned from where it stands.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaxiingDepartureStrandedDoesNotJumpTest,
	"Airside.Model.TaxiingDepartureStrandedDoesNotJump",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiingDepartureStrandedDoesNotJumpTest::RunTest(const FString& Parameters)
{
	// TWO CASES SINCE ISSUE #396. The reported drag moved J ALONG the line, and a rebuild now
	// rejoins an agent onto live pavement still under it (FPlanReResolver's RejoinInPlace) - so
	// that case no longer strands at all, and on its own this test stopped reaching the guard it
	// was written for. The second case strands the route outright, with nothing under the wheels
	// to rejoin, and is the one that still walks the taxi-complete guard's hold.
	for (const bool bDrag : { true, false })
	{
		const TCHAR* Name = bDrag ? TEXT("J dragged along the line") : TEXT("route stranded outright");
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const FPushbackGraph G = PushbackBuildGraph(*Net);
		const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
		if (!TestTrue(FString::Printf(TEXT("%s: parked"), Name), Id > 0)) { return false; }
		if (!TestEqual(FString::Printf(TEXT("%s: departs by pushing back"), Name), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None))
		{
			return false;
		}

		// TAXIING OUT, PAST J: the push ends at E, the taxi out runs E -> J -> B. Past J the current
		// step is J -> B, so moving J strands the route under the wheels.
		auto PastJ = [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			return A != nullptr && A->Phase == EAgentPhase::Taxiing && A->LastMotion.Position.X < 1000.0
				&& A->LastMotion.Position.Y > -9000.0;
		};
		for (int32 Tick = 0; Tick < 30 * 300 && !PastJ(); ++Tick)
		{
			Traffic->Advance(1.0 / 30.0, Net);
		}
		if (!TestTrue(FString::Printf(TEXT("%s: taxiing out, past the junction"), Name), PastJ())) { return false; }

		FVector2D Last = Traffic->FindAgent(Id)->LastMotion.Position;
		if (bDrag)
		{
			Net->RemoveGuidelineNode(G.J);
			const FGuidelineNodeId J2 = TestGraph::Node(*Net, 0.0, -10500.0);
			TestGraph::FJoinOptions Options;
			Options.bDerived = false;
			TestGraph::Join(*Net, G.A, J2, Options);
			TestGraph::Join(*Net, J2, G.B, Options);
			TestGraph::Join(*Net, J2, G.E, Options);
			Traffic->OnGraphRebuilt(*Net);
			TestEqual(FString::Printf(TEXT("%s: the pavement is still under it, so it is NOT stranded (#396)"), Name),
				Traffic->GetLastRebuildSummaryForTest().Stranded, 0);
		}
		else if (!TestTrue(FString::Printf(TEXT("%s: stranded"), Name), FGroundTrafficTestAccess(*Traffic).Strand(Id)))
		{
			return false;
		}

		double WorstJump = 0.0;
		bool bDeparted = false;
		// THE HOLD, COUNTED BY ITS REPLAN rather than seen between frames (issue #444): the held taxi out's replan is
		// UGroundTraffic::RetryWaiters' arm now, at the END of the step the hold began in, so a hold that can be planned
		// at once is over before any frame boundary a test can look at - the aeroplane stands still the same one step
		// (the restart moves it from the next). The attempt counter counts only an agent that IsHoldingForTaxiOut.
		const int32 ReplansBefore = Traffic->TaxiOutReplanAttemptsForTest();
		// ONE SECOND INTO THE ROLL TOO: the take-off puts the aeroplane at its entry on the first
		// frame it ROLLS, the frame after the phase says Departing - a first version stopped at the
		// phase change and passed on the code that jumped (2026-09-27). A second of roll from
		// taxi speed is well under 200 uu a frame.
		int32 RollFrames = 0;
		for (int32 Tick = 0; Tick < 30 * 300 && Traffic->FindAgent(Id) != nullptr && RollFrames < 30; ++Tick)
		{
			Traffic->Advance(1.0 / 30.0, Net);
			const FRoadAgent* Now = Traffic->FindAgent(Id);
			if (Now == nullptr) { break; }
			WorstJump = FMath::Max(WorstJump, FVector2D::Distance(Last, Now->LastMotion.Position));
			Last = Now->LastMotion.Position;
			if (Now->Phase == EAgentPhase::Departing) { bDeparted = true; ++RollFrames; }
		}
		TestTrue(FString::Printf(TEXT("%s: it never jumps: worst frame-to-frame move %.0f uu"), Name, WorstJump), WorstJump < 200.0);
		TestTrue(FString::Printf(TEXT("%s: and it still reaches the runway and rolls"), Name), bDeparted);
		if (!bDrag)
		{
			// THE GUARD THIS TEST EXISTS FOR, reached: a stranded taxi out holds and replans - it is
			// neither Stranded (a departure has a way back: RetryWaiters) nor lined up.
			TestTrue(FString::Printf(TEXT("%s: it went through the taxi-complete guard's hold"), Name),
				Traffic->TaxiOutReplanAttemptsForTest() > ReplansBefore);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A PUSH IS REFUSED INTO A TRUCK THAT IS BACKING OVER ITS GROUND (issue #434).
 *
 * DepartAgent asks IsPushGroundFree, which reads the same claim table every agent's own claims go
 * into. A Reversing truck held NOTHING there (EAgentPhase::Reversing was outside IsOnRoute, so every
 * claim pass gave it HoldRunwayOnly), so the ground under it read free and an aeroplane was cleared
 * to be pushed into it.
 *
 * THE TRUCK BACKS OVER THE FAR ARM - the edge a push reverses onto - and onto the junction the push
 * starts along: F -> E forward, then E -> J as a reverse leg, the same graph and the same aeroplane
 * FPushbackClearanceTest uses, whose blocker is a phantom claim; this is the real thing that
 * phantom stands for. The control is the same push once the truck is gone, so the refusal is shown
 * to be the truck's.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackRefusedIntoABackingTruckTest,
	"Airside.Model.Traffic.PushRefusedIntoABackingTruck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackRefusedIntoABackingTruckTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);

	// Taxied in from the runway, so it parks facing SOUTH with its way out behind it: a push.
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("an aircraft parks facing away from its way out"), Id > 0)) { return false; }

	// THE TRUCK'S ROUTE: in along the far arm, then the last leg backed - the polyline runs E -> J
	// and the body faces the other way. Marked on the plan, as ReverseRunTest marks its legs; the
	// claim pass reads steps, not edges.
	const FGuidelineNodeId F = TestGraph::Node(*Net, 30000.0, -10000.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Net, F, G.E, Options);
	FRoutePlan Route = TestGraph::Probe(*Net, F, G.J, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the truck routes F -> E -> J"), Route.IsValid() && Route.Steps.Num() == 2)) { return false; }
	Route.Steps[1].bReverseLeg = true;

	FVehicle Truck = UAirsideSettings::ResolveDefaultVehicle();
	Truck.Chassis = UAirsideSettings::ResolveLargestServiceVehicle();
	const int32 TruckId = Traffic->DispatchAgent(Net, Route, Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), TruckId > 0)) { return false; }
	if (!TestTrue(TEXT("and starts backing along the far arm"), RunUntil(*Traffic, *Net, 90.0, [&]()
		{
			const FRoadAgent* T = Traffic->FindAgent(TruckId);
			return T != nullptr && T->Phase == EAgentPhase::Reversing;
		}, 1.0 / 30.0))) { return false; }
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestEqual(TEXT("still reversing"), Traffic->FindAgent(TruckId)->Phase, EAgentPhase::Reversing)) { return false; }

	// THE ASSERTION: clearance is withheld while the truck is on the ground the push needs.
	if (!TestEqual(TEXT("a push onto ground a truck is backing over is refused"),
		Traffic->DepartAgent(Id, *Net), EDepartureRefusal::PushbackBlocked))
	{
		return false;
	}
	TestEqual(TEXT("and nothing was half-started"), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked);

	// THE CONTROL: the same push, the same aeroplane, with the truck gone.
	TestTrue(TEXT("the truck retires"), Traffic->RetireAgent(TruckId));
	TestEqual(TEXT("and with it gone the push is cleared - the truck was the reason"),
		Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None);
	return true;
}

/**
 * THE SELECTED AEROPLANE'S ROUTE IS DRAWN DURING ITS PUSH TOO (issue #444). UGroundTraffic::RemainingRouteRuns answered
 * Taxiing and Reversing by name, so for the whole of a push the select tool drew nothing - the one moment an aeroplane
 * is going backwards across the apron. It answers every phase that walks a route now (FAgentPhaseTraits::bOnRoute):
 * the push's own line, as a REVERSE run (the body backs along it), then the taxi out it hands over to, forward.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackRouteIsDrawnTest,
	"Airside.Model.Traffic.PushRouteIsDrawn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackRouteIsDrawnTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);

	// Parked facing south from B -> A, so leaving means a push onto E, then E -> J -> B.
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("parked"), Id > 0)) { return false; }
	if (!TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
	for (int32 Tick = 0; Tick < 10; ++Tick) { Traffic->Advance(1.0 / 30.0, Net); }
	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("still in the manoeuvre"), Agent != nullptr && Agent->Phase == EAgentPhase::Manoeuvring)) { return false; }

	const TArray<FRouteRun> Runs = Traffic->RemainingRouteRuns(Id);
	if (!TestTrue(TEXT("a pushing aeroplane has a route to draw"), Runs.Num() >= 2)) { return false; }
	TestTrue(TEXT("the push is drawn as a reverse run"), Runs[0].bReverse);
	TestTrue(TEXT("along the push's own line"), Runs[0].Points == Agent->Pushback.Plan.Polyline);
	TestFalse(TEXT("then the taxi out, forward"), Runs[1].bReverse);
	TestTrue(TEXT("which begins where the push ends"),
		Runs[1].Points.Num() > 0 && Runs[1].Points[0].Equals(Agent->TaxiOutPlan.Polyline[0], 1.0));
	return true;
}

namespace
{
/**
 * A PUSH WHOSE END IS DELETED NEVER REVERSES ONTO THE RUNWAY (found by #498's probe). The push's re-resolve, finding no node
 * where the push was to end, replanned to the AGENT's goal - the runway entry DepartAgent gave the taxi out - so the push
 * became stand -> junction -> runway entry, and the aeroplane was pushed backwards up the taxiway and onto the runway
 * ("taxi-in replanned by the rebuild at step 1: 20000 uu"). #498's own bug, on the push instead of the taxi out.
 *
 * THE PUSH CONTRACT this states: a push is re-routed only to its OWN end, where its taxi out begins - never to the
 * departure's goal. When that end is gone it is cut back to the last live node it reaches and ends there; a push that ends
 * anywhere but where its taxi out begins holds for a new way to the runway, which RetryWaiters plans from where it stopped.
 *
 * TWO EDITS, one contract: the push's end node deleted, and the arm J-E deleted with E left standing alone (#501 review:
 * the commonest edit, and a different branch - the push's own end is then still a live node, so the search to it runs and
 * finds no way there, where a deleted end's fails at once for want of a goal). Both cut the push back to J.
 */
	bool PushbackStopsShortOfTheRunway(FAutomationTestBase& Test, TFunctionRef<void(URoadNetwork&, const FPushbackGraph&)> Edit)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const FPushbackGraph G = PushbackBuildGraph(*Net);
		const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
		if (!Test.TestTrue(TEXT("parked"), Id > 0)) { return false; }
		if (!Test.TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
		for (int32 Tick = 0; Tick < 30; ++Tick) { Traffic->Advance(1.0 / 30.0, Net); }
		if (!Test.TestTrue(TEXT("mid-push, still on the lead-in"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring
			&& Traffic->FindAgent(Id)->Pushback.Travelled < 10000.0)) { return false; }

		// THE EDIT, beyond the junction. The entry B and the line J-B to it are untouched.
		const FVector2D Entry = Net->GetGuidelineNode(G.B)->Position;
		const FVector2D Junction = Net->GetGuidelineNode(G.J)->Position;
		const double PushNearest = FVector2D::Distance(Junction, Entry);   // the nearest a push down the lead-in comes to B
		Edit(*Net, G);
		FLogLineSpy Spy(FName(TEXT("LogAirsideTraffic")));
		GLog->AddOutputDevice(&Spy);
		Traffic->OnGraphRebuilt(*Net);
		GLog->RemoveOutputDevice(&Spy);
		Test.TestTrue(TEXT("the rebuild's line names the push, not a taxi-in"), Spy.CapturedLines.ContainsByPredicate([](const FString& L)
			{
				return L.Contains(TEXT("'s push truncated by the rebuild"));
			}));

		auto PushNamesTheEntry = [&](const FRoadAgent& A)
		{
			return A.Pushback.Plan.Steps.ContainsByPredicate([&](const FRouteStep& S) { return S.To == G.B; })
				|| A.Pushback.Plan.Polyline.ContainsByPredicate([&](const FVector2D& P) { return FVector2D::Distance(P, Entry) < 100.0; });
		};
		const FRoadAgent* Rebuilt = Traffic->FindAgent(Id);
		Test.TestFalse(TEXT("the push is not re-routed to the runway entry"), PushNamesTheEntry(*Rebuilt));
		Test.TestTrue(FString::Printf(TEXT("it is cut back to the junction, the last live node it reaches (ends %.0f uu from it)"),
			Rebuilt->Pushback.Plan.Polyline.Num() > 0 ? FVector2D::Distance(Rebuilt->Pushback.Plan.Polyline.Last(), Junction) : -1.0),
			Rebuilt->Pushback.Plan.IsValid() && FVector2D::Distance(Rebuilt->Pushback.Plan.Polyline.Last(), Junction) < 1.0);

		// THE PUSH PLAYS OUT: never along a line to the entry, never nearer it than the junction.
		double Nearest = TNumericLimits<double>::Max();
		bool bNamed = false;
		FVector2D PushEndedAt = Rebuilt->LastMotion.Position;
		const bool bPushOver = RunUntil(*Traffic, *Net, 300.0, [&]()
			{
				const FRoadAgent* A = Traffic->FindAgent(Id);
				if (A == nullptr || A->Phase != EAgentPhase::Manoeuvring || A->IsHoldingForTaxiOut()) { return true; }
				Nearest = FMath::Min(Nearest, FVector2D::Distance(A->LastMotion.Position, Entry));
				bNamed |= PushNamesTheEntry(*A);
				PushEndedAt = A->LastMotion.Position;
				return false;
			}, 1.0 / 30.0);
		Test.TestTrue(TEXT("the push ends"), bPushOver);
		Test.TestFalse(TEXT("its push never named the entry"), bNamed);
		Test.TestTrue(FString::Printf(TEXT("and never took it nearer the entry than the junction (%.0f uu, the junction is %.0f)"), Nearest, PushNearest),
			Nearest >= PushNearest - 10.0);
		Test.TestTrue(FString::Printf(TEXT("it ends at the junction (%.0f uu from it)"), FVector2D::Distance(PushEndedAt, Junction)),
			FVector2D::Distance(PushEndedAt, Junction) < 10.0);

		// AND IS GIVEN A WAY OUT FROM THERE: J-B is live, so the retry plans it at once, and it rolls.
		Test.TestTrue(TEXT("then a new way to the runway from where its push ended, and it rolls"), RunUntil(*Traffic, *Net, 300.0, [&]()
			{
				const FRoadAgent* A = Traffic->FindAgent(Id);
				return A != nullptr && A->Phase == EAgentPhase::Departing;
			}, 1.0 / 30.0));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackEndGoneStopsShortTest,
	"Airside.Model.PushbackEndGoneStopsShortOfTheRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackEndGoneStopsShortTest::RunTest(const FString& Parameters)
{
	// THE PUSH'S END GOES: E, and the arm J-E with it.
	return PushbackStopsShortOfTheRunway(*this, [](URoadNetwork& Net, const FPushbackGraph& G) { Net.RemoveGuidelineNode(G.E); });
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackArmDeletedStopsShortTest,
	"Airside.Model.PushbackArmDeletedStopsAtTheJunction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackArmDeletedStopsShortTest::RunTest(const FString& Parameters)
{
	// THE ARM GOES, ITS END LEFT STANDING ALONE: the line J-E, not the node E.
	return PushbackStopsShortOfTheRunway(*this, [](URoadNetwork& Net, const FPushbackGraph& G)
		{
			for (int32 Index = 0; Index < Net.GetGuidelineEdges().Num(); ++Index)
			{
				const FGuidelineEdge& Edge = Net.GetGuidelineEdges()[Index];
				if (Edge.bAlive && ((Edge.A == G.J && Edge.B == G.E) || (Edge.A == G.E && Edge.B == G.J)))
				{
					FGuidelineEdgeId JE;
					JE.Index = Index;
					JE.Generation = Edge.Generation;
					Net.RemoveGuidelineEdge(JE);
					return;
				}
			}
		});
}

/**
 * A PUSH WHOSE JUNCTION IS DRAGGED BEHIND IT COMPLETES (#501 review). Past J and pushed along the arm toward E, J moves
 * 5 m: far past the 25 uu re-resolve radius, so the step it is on fails its match with the arm under it still pavement, a
 * few metres off. Stranding it stopped the aeroplane mid-arm for good - no node within the held taxi out's 30 m - where
 * main had played its old line out over the pavement and departed. It rejoins the moved arm (ReResolvePlan's RejoinPush)
 * by a join leg, not a hop, completes its push at E and departs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackJunctionMovedBehindItTest,
	"Airside.Model.PushbackJunctionMovedBehindItCompletes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackJunctionMovedBehindItTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("parked"), Id > 0)) { return false; }
	if (!TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
	// 6 km ALONG THE ARM: the nearest node, after the drag, is 60 m away - past the held taxi out's 30 m reach.
	for (int32 Tick = 0; Tick < 30 * 300 && Traffic->FindAgent(Id)->Pushback.Travelled < 16000.0; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("pushing along the arm, past the junction"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring
		&& Traffic->FindAgent(Id)->Pushback.Travelled >= 16000.0)) { return false; }

	// J DRAGGED 5 m SOUTH: every line through it re-laid to the moved node. E, the push's end, stays.
	const FVector2D EndAt = Net->GetGuidelineNode(G.E)->Position;
	Net->RemoveGuidelineNode(G.J);
	const FGuidelineNodeId J2 = TestGraph::Node(*Net, 0.0, -10500.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Net, G.A, J2, Options);
	TestGraph::Join(*Net, J2, G.B, Options);
	TestGraph::Join(*Net, J2, G.E, Options);
	// A WAIT IT WAS IN when the edit landed (#501 re-review): somebody's claim on the push's end, five seconds of stall.
	// The rejoin is a new route, and #429's reason holds for it - the wait is over by construction - so both reset.
	FGroundTrafficTestAccess(*Traffic).ScriptWait(Id, FTrafficResource::OfNode(G.E), 99, 5.0);
	FLogLineSpy Spy(FName(TEXT("LogAirsideTraffic")));
	GLog->AddOutputDevice(&Spy);
	Traffic->OnGraphRebuilt(*Net);
	GLog->RemoveOutputDevice(&Spy);
	TestTrue(TEXT("the push rejoined the moved arm"), Spy.CapturedLines.ContainsByPredicate([](const FString& L)
		{
			return L.Contains(TEXT("'s push rejoined the pavement under it"));
		}));
	TestTrue(TEXT("its push is live, not stranded"), Traffic->FindAgent(Id)->Pushback.Plan.IsValid());
	TestTrue(FString::Printf(TEXT("and its old wait is over: waiting on %d, stalled %.1f s"), Traffic->FindAgent(Id)->GetWaitingOn(),
		Traffic->FindAgent(Id)->GetStalledSeconds()),
		Traffic->FindAgent(Id)->GetWaitingOn() == 0 && Traffic->FindAgent(Id)->GetStalledSeconds() == 0.0);

	// THE PUSH PLAYS OUT, without a jump or a swing, to its own end. The join leg meets the moved arm at atan(1/4), 14.04
	// degrees, and leaves the old line at that less the arm's own 1.4 - the bound FPushbackRun::Rejoin states.
	FVector2D Last = Traffic->FindAgent(Id)->LastMotion.Position;
	double LastHeading = Traffic->FindAgent(Id)->LastMotion.Heading;
	FVector2D PushEndedAt = Last;
	double WorstJump = 0.0;
	double WorstTurn = 0.0;
	RunUntil(*Traffic, *Net, 300.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			if (A == nullptr || A->Phase != EAgentPhase::Manoeuvring || A->IsHoldingForTaxiOut()) { return true; }
			WorstJump = FMath::Max(WorstJump, FVector2D::Distance(Last, A->LastMotion.Position));
			WorstTurn = FMath::Max(WorstTurn,
				FMath::RadiansToDegrees(FMath::Abs(FMath::UnwindRadians(A->LastMotion.Heading - LastHeading))));
			Last = A->LastMotion.Position;
			LastHeading = A->LastMotion.Heading;
			PushEndedAt = Last;
			return false;
		}, 1.0 / 30.0);
	TestTrue(FString::Printf(TEXT("it never jumps onto the moved arm: worst frame-to-frame move %.0f uu"), WorstJump), WorstJump < 200.0);
	TestTrue(FString::Printf(TEXT("nor swings onto it: worst frame-to-frame turn %.2f deg"), WorstTurn), WorstTurn <= 14.1);
	TestTrue(FString::Printf(TEXT("it completes its push at E (%.0f uu from it)"), FVector2D::Distance(PushEndedAt, EndAt)),
		FVector2D::Distance(PushEndedAt, EndAt) < 10.0);
	TestTrue(TEXT("and departs"), RunUntil(*Traffic, *Net, 300.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			return A != nullptr && A->Phase == EAgentPhase::Departing;
		}, 1.0 / 30.0));
	return true;
}

/**
 * A PUSH WHOSE LEAD-IN IS SHORTENED UNDER IT COMPLETES (#501 review, the probe's S6). On the lead-in A-J, J and E are both
 * moved 5 m: J along the lead-in's own axis, so the line under the aeroplane is still pavement exactly where it is, but the
 * step it is on ends at a node that no longer resolves and the push's end E has gone. It held where it stood with the line
 * still under it - no node within 30 m. It rejoins the line it is on (no join: 0 uu off), completes its push at the moved
 * end, and departs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackLeadInMovedAlongItTest,
	"Airside.Model.PushbackLeadInMovedAlongItCompletes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackLeadInMovedAlongItTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("parked"), Id > 0)) { return false; }
	if (!TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
	// HALF WAY DOWN THE LEAD-IN: 50 m from the stand and 45 m from where J will be - both past the retry's 30 m.
	for (int32 Tick = 0; Tick < 30 * 300 && Traffic->FindAgent(Id)->Pushback.Travelled < 5000.0; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("pushing down the lead-in"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring
		&& Traffic->FindAgent(Id)->Pushback.Travelled >= 5000.0 && Traffic->FindAgent(Id)->Pushback.Travelled < 10000.0)) { return false; }

	Net->RemoveGuidelineNode(G.J);
	Net->RemoveGuidelineNode(G.E);
	const FGuidelineNodeId J2 = TestGraph::Node(*Net, 0.0, -10500.0);
	const FGuidelineNodeId E2 = TestGraph::Node(*Net, 20000.0, -10500.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Net, G.A, J2, Options);
	TestGraph::Join(*Net, J2, G.B, Options);
	TestGraph::Join(*Net, J2, E2, Options);
	const FVector2D FoundAt = Traffic->FindAgent(Id)->LastMotion.Position;
	const double TravelledWas = Traffic->FindAgent(Id)->Pushback.Travelled;
	Traffic->OnGraphRebuilt(*Net);
	const FRoadAgent* Rebuilt = Traffic->FindAgent(Id);
	TestTrue(TEXT("its push is live, not stranded"), Rebuilt->Pushback.Plan.IsValid());
	// ON THE SAME LINE FROM THE SAME STAND, so the projection is as far along the rejoined push as the aeroplane was along
	// the old one: no join leg, and nothing re-read on other geometry.
	TestTrue(FString::Printf(TEXT("it rejoins at the distance it had pushed (%.1f uu, was %.1f)"), Rebuilt->Pushback.Travelled,
		TravelledWas), FMath::IsNearlyEqual(Rebuilt->Pushback.Travelled, TravelledWas, 1.0));
	Traffic->Advance(1.0 / 30.0, Net);
	TestTrue(FString::Printf(TEXT("and one frame on it is a frame's push from where it stood (%.1f uu)"),
		FVector2D::Distance(FoundAt, Traffic->FindAgent(Id)->LastMotion.Position)),
		FVector2D::Distance(FoundAt, Traffic->FindAgent(Id)->LastMotion.Position) < 20.0);

	FVector2D PushEndedAt = Traffic->FindAgent(Id)->LastMotion.Position;
	RunUntil(*Traffic, *Net, 300.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			if (A == nullptr || A->Phase != EAgentPhase::Manoeuvring || A->IsHoldingForTaxiOut()) { return true; }
			PushEndedAt = A->LastMotion.Position;
			return false;
		}, 1.0 / 30.0);
	const FVector2D MovedEnd = Net->GetGuidelineNode(E2)->Position;
	TestTrue(FString::Printf(TEXT("it completes its push at the moved end (%.0f uu from it)"), FVector2D::Distance(PushEndedAt, MovedEnd)),
		FVector2D::Distance(PushEndedAt, MovedEnd) < 10.0);
	TestTrue(TEXT("and departs"), RunUntil(*Traffic, *Net, 300.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			return A != nullptr && A->Phase == EAgentPhase::Departing;
		}, 1.0 / 30.0));
	return true;
}

namespace
{
	/**
	 * A REJOIN WITH NO ROOM AHEAD HOLDS INSTEAD (#501 re-review), the measure both shapes below share. A push has no steer
	 * law, so the join leg's angle is an instant yaw: FPushbackRun::Rejoin refuses a join its first step has not four
	 * offsets of room for, and the push strands and holds. Here: no rejoin, the push stranded, the aeroplane not a uu
	 * further while it is still the push's, no frame turning it past the join's own 14.04 degrees, and the hold's retry -
	 * "planned again from where it was pushed back to", said only for a holding push - giving it a way out from there.
	 * Moving J and E is a drag of both, 5 m south; PushTo is where along the push the edit lands. bMoveEnd false drags J
	 * alone, so the push's end E still resolves (#502): the case the rebuild used to splice at the push's own step.
	 */
	bool PushbackRejoinWithNoRoomHolds(FAutomationTestBase& Test, double PushTo, bool bMoveEnd = true)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const FPushbackGraph G = PushbackBuildGraph(*Net);
		const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
		if (!Test.TestTrue(TEXT("parked"), Id > 0)) { return false; }
		if (!Test.TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
		for (int32 Tick = 0; Tick < 30 * 600 && Traffic->FindAgent(Id)->Pushback.Travelled < PushTo; ++Tick)
		{
			Traffic->Advance(1.0 / 30.0, Net);
		}
		const FRoadAgent* Pushing = Traffic->FindAgent(Id);
		if (!Test.TestTrue(FString::Printf(TEXT("still pushing, %.0f uu along"), Pushing->Pushback.Travelled),
			Pushing->Phase == EAgentPhase::Manoeuvring && Pushing->Pushback.Travelled >= PushTo
			&& !Pushing->Pushback.HasArrived())) { return false; }
		const FVector2D StoodAt = Pushing->LastMotion.Position;
		double LastHeading = Pushing->LastMotion.Heading;

		Net->RemoveGuidelineNode(G.J);
		if (bMoveEnd)
		{
			Net->RemoveGuidelineNode(G.E);
		}
		const FGuidelineNodeId J2 = TestGraph::Node(*Net, 0.0, -10500.0);
		const FGuidelineNodeId E2 = bMoveEnd ? TestGraph::Node(*Net, 20000.0, -10500.0) : G.E;
		TestGraph::FJoinOptions Options;
		Options.bDerived = false;
		TestGraph::Join(*Net, G.A, J2, Options);
		TestGraph::Join(*Net, J2, G.B, Options);
		TestGraph::Join(*Net, J2, E2, Options);
		FLogLineSpy Spy(FName(TEXT("LogAirsideTraffic")));
		GLog->AddOutputDevice(&Spy);
		Traffic->OnGraphRebuilt(*Net);
		const FRoadAgent* Rebuilt = Traffic->FindAgent(Id);
		// NOT SPLICED AT ITS OWN STEP (#502): with its end live the rebuild searched A -> J2 -> E and re-read the push's
		// Travelled along it - a sideways hop and a swing in one frame, the moves the rest of this measures.
		Test.TestFalse(TEXT("its push is not spliced at the step it is on"),
			Spy.CapturedLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("'s push replanned by the rebuild")); }));
		Test.TestFalse(TEXT("its push does not rejoin: the join would have had no room"),
			Spy.CapturedLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("'s push rejoined")); }));
		Test.TestEqual(TEXT("its push is stranded"), Rebuilt->Pushback.Plan.Result, ERouteResult::Unreachable);

		double WorstMove = 0.0;
		double WorstTurn = 0.0;
		const bool bDeparted = RunUntil(*Traffic, *Net, 300.0, [&]()
			{
				const FRoadAgent* A = Traffic->FindAgent(Id);
				if (A != nullptr && A->Phase == EAgentPhase::Manoeuvring)
				{
					WorstMove = FMath::Max(WorstMove, FVector2D::Distance(StoodAt, A->LastMotion.Position));
					WorstTurn = FMath::Max(WorstTurn,
						FMath::RadiansToDegrees(FMath::Abs(FMath::UnwindRadians(A->LastMotion.Heading - LastHeading))));
					LastHeading = A->LastMotion.Heading;
				}
				return A != nullptr && A->Phase == EAgentPhase::Departing;
			}, 1.0 / 30.0);
		GLog->RemoveOutputDevice(&Spy);
		Test.TestTrue(FString::Printf(TEXT("it stops where the edit found it (%.1f uu further at most)"), WorstMove), WorstMove < 1.0);
		Test.TestTrue(FString::Printf(TEXT("and never swings while it is the push's: worst frame-to-frame turn %.2f deg"), WorstTurn),
			WorstTurn <= 14.1);
		Test.TestTrue(TEXT("it held there, and the hold's retry planned it a way out from where it stopped"),
			Spy.CapturedLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("planned again from where it was pushed back to")); }));
		Test.TestTrue(TEXT("and it departs"), bDeparted);
		return true;
	}
}

/**
 * AN ARM SHIFTED SIDEWAYS UNDER A PUSH NEAR ITS END HOLDS (#501 re-review). The moved arm is 5 m off and the push 10 m from
 * its end: the join, clamped to the step's end, met the line at 26.6 degrees in one frame (68 at 2 m). No room for a join
 * four offsets long, so no join.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackArmShiftedNearItsEndTest,
	"Airside.Model.PushbackArmShiftedNearItsEndHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackArmShiftedNearItsEndTest::RunTest(const FString& Parameters)
{
	return PushbackRejoinWithNoRoomHolds(*this, 29000.0);
}

/**
 * A LEAD-IN MOVED BEHIND A PUSH THAT HAS PASSED THE MOVED NODE HOLDS (#501 re-review). J moves 5 m back along the lead-in to
 * just behind the aeroplane: its projection on the shortened lead-in is that edge's END, and the join ran BACKWARD to it - a
 * 180-degree flip in one frame. A join needs room ahead; there is none behind.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackPastTheMovedNodeTest,
	"Airside.Model.PushbackPastTheMovedNodeHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackPastTheMovedNodeTest::RunTest(const FString& Parameters)
{
	return PushbackRejoinWithNoRoomHolds(*this, 9750.0);
}

/**
 * THE SAME, WITH THE PUSH'S END LEFT WHERE IT WAS (#502; the probe's S6 geometry with the goal surviving). J alone moves 5 m
 * back along the lead-in, to just behind the aeroplane; E still resolves. The rebuild searched A -> J2 -> E from the step the
 * push was on and spliced it there with Travelled kept: 9750 uu re-read on a lead-in now 9500 long put the aeroplane 250 uu
 * along the arm - a hop sideways and a quarter turn in one frame (the #501 re-review traced 570 uu in S6). A push is spliced
 * only AHEAD of its own step, as a taxi is; under it, it rejoins or holds, and here the rejoin has no room.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackPastTheMovedNodeEndKeptTest,
	"Airside.Model.PushbackPastTheMovedNodeEndKeptHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackPastTheMovedNodeEndKeptTest::RunTest(const FString& Parameters)
{
	return PushbackRejoinWithNoRoomHolds(*this, 9750.0, /*bMoveEnd*/ false);
}

/**
 * A PUSH ON GROUND THE EDIT DELETED STOPS (found by #498's probe). The rebuild strands a push whose step under it is gone -
 * its plan Unreachable - and FPushbackRun went on playing it: Travelled grew along the deleted lead-in for the rest of the
 * push (17778 uu two minutes on) and the aeroplane then taxied out as if nothing had happened. A taxiing departure whose
 * route dies stops where it is and holds for a new way to the runway (TaxiingDepartureStrandedDoesNotJump); a push is that
 * departure's first leg, and gets the same: at rest where the edit found it, holding, until a line from there exists.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackOnDeletedGroundStopsTest,
	"Airside.Model.PushbackOnDeletedGroundStops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackOnDeletedGroundStopsTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("parked"), Id > 0)) { return false; }
	if (!TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
	for (int32 Tick = 0; Tick < 30; ++Tick) { Traffic->Advance(1.0 / 30.0, Net); }
	if (!TestTrue(TEXT("mid-push, on the lead-in"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring
		&& Traffic->FindAgent(Id)->Pushback.Travelled < 10000.0)) { return false; }

	// THE LEAD-IN UNDER IT GOES: the line A-J it is being pushed along.
	FGuidelineEdgeId AJ;
	for (int32 Index = 0; Index < Net->GetGuidelineEdges().Num(); ++Index)
	{
		const FGuidelineEdge& Edge = Net->GetGuidelineEdges()[Index];
		if (Edge.bAlive && ((Edge.A == G.A && Edge.B == G.J) || (Edge.A == G.J && Edge.B == G.A)))
		{
			AJ.Index = Index;
			AJ.Generation = Edge.Generation;
		}
	}
	if (!TestTrue(TEXT("the lead-in exists to delete"), AJ.IsSet())) { return false; }
	Net->RemoveGuidelineEdge(AJ);
	Traffic->OnGraphRebuilt(*Net);
	const FRoadAgent* Rebuilt = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("precondition: the rebuild stranded the push - its plan is dead"),
		Rebuilt->Phase == EAgentPhase::Manoeuvring && Rebuilt->Pushback.Plan.Result == ERouteResult::Unreachable)) { return false; }
	const double TravelledAt = Rebuilt->Pushback.Travelled;
	const FVector2D FoundAt = Rebuilt->LastMotion.Position;

	for (int32 Tick = 0; Tick < 30 * 10; ++Tick) { Traffic->Advance(1.0 / 30.0, Net); }
	const FRoadAgent* After = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("it is still there"), After)) { return false; }
	TestTrue(FString::Printf(TEXT("it pushes no further along the deleted line (Travelled %.0f -> %.0f)"), TravelledAt, After->Pushback.Travelled),
		After->Pushback.Travelled <= TravelledAt + UE_KINDA_SMALL_NUMBER);
	TestTrue(FString::Printf(TEXT("it stands where the edit found it (%.2f uu away)"), FVector2D::Distance(After->LastMotion.Position, FoundAt)),
		FVector2D::Distance(After->LastMotion.Position, FoundAt) < 1.0);
	TestTrue(FString::Printf(TEXT("at rest (%.2f uu/s)"), After->LastMotion.GroundSpeed), FMath::IsNearlyZero(After->LastMotion.GroundSpeed));
	TestTrue(TEXT("holding for a new way to the runway, as a taxiing departure whose route died does"), After->IsHoldingForTaxiOut());

	// A SECOND EDIT, NOWHERE NEAR IT, leaves it a departure (#501 review): the rebuild's push arm pointed the goal at the
	// DEAD push's end before skipping it, and the card's destination went from its runway to a node.
	FAgentFacts Facts;
	InspectFacts::DescribeAgent(*Traffic, Net, Id, Facts);
	TestTrue(FString::Printf(TEXT("held, its card names the runway (%s)"), *Facts.Destination), Facts.Destination.StartsWith(TEXT("Runway")));
	const FGuidelineNodeId GoalBefore = After->GoalNode;
	TestGraph::Node(*Net, -30000.0, -30000.0);
	Traffic->OnGraphRebuilt(*Net);
	TestTrue(TEXT("an unrelated rebuild leaves its goal where it was"), Traffic->FindAgent(Id)->GoalNode == GoalBefore);
	InspectFacts::DescribeAgent(*Traffic, Net, Id, Facts);
	TestTrue(FString::Printf(TEXT("and its card still names the runway (%s)"), *Facts.Destination), Facts.Destination.StartsWith(TEXT("Runway")));

	// THE PLAYER REDRAWS THE LEAD-IN: a way out from where it holds, and it goes.
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Net, G.A, G.J, Options);
	TestTrue(TEXT("given a way out once the line is back, it rolls"), RunUntil(*Traffic, *Net, 300.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			return A != nullptr && A->Phase == EAgentPhase::Departing;
		}, 1.0 / 30.0));
	return true;
}

namespace
{
	/** The live guideline edge joining A and B either way round, or unset: for an edit that deletes one line. */
	FGuidelineEdgeId PushbackEdgeBetween(const URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B)
	{
		for (int32 Index = 0; Index < Net.GetGuidelineEdges().Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Net.GetGuidelineEdges()[Index];
			if (Edge.bAlive && ((Edge.A == A && Edge.B == B) || (Edge.A == B && Edge.B == A)))
			{
				return Net.GuidelineEdgeIdAt(Index);
			}
		}
		return FGuidelineEdgeId();
	}
}

/**
 * A PUSH WHOSE CURRENT STEP IS DELETED, ITS END STILL LIVE, STOPS WHERE IT STANDS (#502). Pushed along the arm past J, the
 * arm J-E goes and the player draws J-K-E round it. E still resolves, so the rebuild searched J -> K -> E and spliced it at
 * the push's own step with Travelled kept: the same 6 km re-read along J-K put the aeroplane metres sideways in one frame
 * and swung it through the angle between the two lines. A push is spliced only AHEAD of its own step, as a taxi is
 * (ReResolvePlan's bDriving branch): under it, the push rejoins live pavement within 10 m or holds - and here nothing live
 * runs its way within 10 m, so it holds where it stands.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackCurrentStepDeletedHoldsTest,
	"Airside.Model.PushbackCurrentStepDeletedHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackCurrentStepDeletedHoldsTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("parked"), Id > 0)) { return false; }
	if (!TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
	for (int32 Tick = 0; Tick < 30 * 300 && Traffic->FindAgent(Id)->Pushback.Travelled < 16000.0; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("pushing along the arm, past the junction"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring
		&& Traffic->FindAgent(Id)->Pushback.Travelled >= 16000.0 && !Traffic->FindAgent(Id)->Pushback.HasArrived())) { return false; }
	const FVector2D StoodAt = Traffic->FindAgent(Id)->LastMotion.Position;
	double LastHeading = Traffic->FindAgent(Id)->LastMotion.Heading;

	// THE ARM UNDER IT GOES; A WAY ROUND TO ITS END IS DRAWN. E is untouched, so the push's end still resolves.
	const FGuidelineEdgeId JE = PushbackEdgeBetween(*Net, G.J, G.E);
	if (!TestTrue(TEXT("the arm exists to delete"), JE.IsSet())) { return false; }
	Net->RemoveGuidelineEdge(JE);
	const FGuidelineNodeId K = TestGraph::Node(*Net, 10000.0, -15000.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Net, G.J, K, Options);
	TestGraph::Join(*Net, K, G.E, Options);
	FLogLineSpy Spy(FName(TEXT("LogAirsideTraffic")));
	GLog->AddOutputDevice(&Spy);
	Traffic->OnGraphRebuilt(*Net);
	GLog->RemoveOutputDevice(&Spy);
	TestFalse(TEXT("its push is not spliced at the step it is on"), Spy.CapturedLines.ContainsByPredicate([](const FString& L)
		{
			return L.Contains(TEXT("'s push replanned by the rebuild"));
		}));
	TestEqual(TEXT("its push is stranded: nothing live within 10 m runs its way"),
		Traffic->FindAgent(Id)->Pushback.Plan.Result, ERouteResult::Unreachable);

	double WorstMove = 0.0;
	double WorstTurn = 0.0;
	for (int32 Tick = 0; Tick < 30 * 10; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		const FRoadAgent* A = Traffic->FindAgent(Id);
		if (A == nullptr) { break; }
		WorstMove = FMath::Max(WorstMove, FVector2D::Distance(StoodAt, A->LastMotion.Position));
		WorstTurn = FMath::Max(WorstTurn, FMath::RadiansToDegrees(FMath::Abs(FMath::UnwindRadians(A->LastMotion.Heading - LastHeading))));
		LastHeading = A->LastMotion.Heading;
	}
	TestTrue(FString::Printf(TEXT("it stays where the edit found it (%.1f uu away at most)"), WorstMove), WorstMove < 1.0);
	TestTrue(FString::Printf(TEXT("and never swings: worst frame-to-frame turn %.2f deg"), WorstTurn), WorstTurn <= 14.1);
	const FRoadAgent* After = Traffic->FindAgent(Id);
	TestTrue(TEXT("holding there for a way out, as a taxiing departure whose route died does"),
		After != nullptr && After->Phase == EAgentPhase::Manoeuvring && After->IsHoldingForTaxiOut());
	return true;
}

namespace
{
	/**
	 * A PUSH RE-ROUTED TO ITS LIVE END, BY ITS OWN KIND OF ROUTE (#502). Mid lead-in, the arm J-E is deleted and Draw lays
	 * another way from J to E; E still resolves, so the rebuild searches the push a new way there. It searched by the
	 * rebuild's own errand - a free runway is ordinary line under Held, and no detour is too long - where the push was
	 * planned by PushbackClear (no runway at all) and granted whole by DepartAgent for the length it was. bRefused: the new
	 * way is refused, the push is cut back to J, ends there and holds, and the hold's retry gives it a way out from J.
	 * Otherwise the push completes at E. Either way it never names a runway edge.
	 */
	bool PushbackReRoutedToItsEnd(FAutomationTestBase& Test, TFunctionRef<void(URoadNetwork&, const FPushbackGraph&)> Draw,
		bool bRefused)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const FPushbackGraph G = PushbackBuildGraph(*Net);
		const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
		if (!Test.TestTrue(TEXT("parked"), Id > 0)) { return false; }
		if (!Test.TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
		for (int32 Tick = 0; Tick < 30; ++Tick) { Traffic->Advance(1.0 / 30.0, Net); }
		if (!Test.TestTrue(TEXT("mid-push, still on the lead-in"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring
			&& Traffic->FindAgent(Id)->Pushback.Travelled < 10000.0)) { return false; }

		const FVector2D Junction = Net->GetGuidelineNode(G.J)->Position;
		const FVector2D End = Net->GetGuidelineNode(G.E)->Position;
		const FGuidelineEdgeId JE = PushbackEdgeBetween(*Net, G.J, G.E);
		if (!Test.TestTrue(TEXT("the arm exists to delete"), JE.IsSet())) { return false; }
		Net->RemoveGuidelineEdge(JE);
		Draw(*Net, G);
		Traffic->OnGraphRebuilt(*Net);

		auto NamesARunway = [&](const FRoadAgent& A)
		{
			return A.Pushback.Plan.Steps.ContainsByPredicate([&](const FRouteStep& Step)
				{
					const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Step.Edge);
					return Edge != nullptr && Net->IsRunwaySegment(Edge->DerivedFrom);
				});
		};
		const FRoadAgent* Rebuilt = Traffic->FindAgent(Id);
		const FVector2D Wanted = bRefused ? Junction : End;
		const FVector2D Ends = Rebuilt->Pushback.Plan.Polyline.Num() > 0 ? Rebuilt->Pushback.Plan.Polyline.Last() : FVector2D::ZeroVector;
		Test.TestTrue(FString::Printf(TEXT("the push ends %s (%.0f uu from it, %.0f uu long)"),
			bRefused ? TEXT("at J, cut back: the way round is refused") : TEXT("at E, by the way round"),
			FVector2D::Distance(Ends, Wanted), Rebuilt->Pushback.Plan.Length),
			Rebuilt->Pushback.Plan.IsValid() && FVector2D::Distance(Ends, Wanted) < 1.0);
		Test.TestFalse(TEXT("the push names no runway edge"), NamesARunway(*Rebuilt));

		FVector2D PushEndedAt = Rebuilt->LastMotion.Position;
		bool bOnRunway = false;
		RunUntil(*Traffic, *Net, 600.0, [&]()
			{
				const FRoadAgent* A = Traffic->FindAgent(Id);
				if (A == nullptr || A->Phase != EAgentPhase::Manoeuvring || A->IsHoldingForTaxiOut()) { return true; }
				bOnRunway |= NamesARunway(*A);
				PushEndedAt = A->LastMotion.Position;
				return false;
			}, 1.0 / 30.0);
		Test.TestFalse(TEXT("nor ever does while it plays out"), bOnRunway);
		Test.TestTrue(FString::Printf(TEXT("it ends its push %s (%.0f uu from it)"), bRefused ? TEXT("at J") : TEXT("at E"),
			FVector2D::Distance(PushEndedAt, Wanted)), FVector2D::Distance(PushEndedAt, Wanted) < 10.0);
		Test.TestTrue(TEXT("and departs"), RunUntil(*Traffic, *Net, 600.0, [&]()
			{
				const FRoadAgent* A = Traffic->FindAgent(Id);
				return A != nullptr && A->Phase == EAgentPhase::Departing;
			}, 1.0 / 30.0));
		return true;
	}
}

/**
 * THE ONLY WAY ROUND RUNS ALONG A FREE RUNWAY: REFUSED, AND THE PUSH HOLDS AT J (#502). J -> M is taxiway; M -> E is a line
 * of the strip. Free, it was the rebuild errand's ordinary line, and the aeroplane was pushed backwards down a runway.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackReRouteAlongARunwayTest,
	"Airside.Model.PushbackReRouteAlongAFreeRunwayHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackReRouteAlongARunwayTest::RunTest(const FString& Parameters)
{
	return PushbackReRoutedToItsEnd(*this, [](URoadNetwork& Net, const FPushbackGraph& G)
		{
			FRoadSegmentId Strip;
			for (int32 Index = 0; Index < Net.GetSegments().Num() && !Strip.IsSet(); ++Index)
			{
				const FRoadSegmentId Segment = Net.SegmentIdAt(Index);
				Strip = Net.IsRunwaySegment(Segment) ? Segment : FRoadSegmentId();
			}
			const FGuidelineNodeId M = TestGraph::Node(Net, 10000.0, -10000.0);
			TestGraph::FJoinOptions Options;
			Options.bDerived = false;
			TestGraph::Join(Net, G.J, M, Options);
			FGuidelineEdge Along;
			Along.A = M;
			Along.B = G.E;
			Along.Control = FVector2D(15000.0, -10000.0);
			Along.AllowedTraffic = FTrafficMask::All();
			Along.DerivedFrom = Strip;
			Net.AddGuidelineEdge(MoveTemp(Along));
		}, /*bRefused*/ true);
}

/**
 * THE ONLY WAY ROUND IS THREE TIMES THE ARM: REFUSED, AND THE PUSH HOLDS AT J (#502). A push is granted whole by DepartAgent
 * for the ground it will cover; 630 m of backing round a detour is a different manoeuvre nobody cleared.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackReRouteLongDetourTest,
	"Airside.Model.PushbackReRouteLongDetourHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackReRouteLongDetourTest::RunTest(const FString& Parameters)
{
	return PushbackReRoutedToItsEnd(*this, [](URoadNetwork& Net, const FPushbackGraph& G)
		{
			const FGuidelineNodeId K = TestGraph::Node(Net, 10000.0, -40000.0);
			TestGraph::FJoinOptions Options;
			Options.bDerived = false;
			TestGraph::Join(Net, G.J, K, Options);
			TestGraph::Join(Net, K, G.E, Options);
		}, /*bRefused*/ true);
}

/**
 * THE CONTROL: A WAY ROUND ABOUT AS LONG AS THE ARM, ON NO RUNWAY, IS TAKEN (#502). The arm re-laid through M2, 5 m off its
 * old line - 25 uu longer. Without it, a re-route that refused everything would pass the two above.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackReRouteShortWayTest,
	"Airside.Model.PushbackReRouteShortWayCompletes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackReRouteShortWayTest::RunTest(const FString& Parameters)
{
	return PushbackReRoutedToItsEnd(*this, [](URoadNetwork& Net, const FPushbackGraph& G)
		{
			const FGuidelineNodeId M2 = TestGraph::Node(Net, 10000.0, -10500.0);
			TestGraph::FJoinOptions Options;
			Options.bDerived = false;
			TestGraph::Join(Net, G.J, M2, Options);
			TestGraph::Join(Net, M2, G.E, Options);
		}, /*bRefused*/ false);
}

#endif
