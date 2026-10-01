#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AirsideTestsLog.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficClaims.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------
// Issue #256: the 2026-09-21 review's five criticals were all structural findings on the
// ten-node fixtures every other Airside test builds - correct at that scale, and never
// MEASURED at the scale a built-out airport reaches. This file is the one fixture that
// measures cost rather than correctness: FTestAirport::BuildScale (AirsideTestFixtures.h/
// .cpp) lays two runways, an 8x20 taxiway grid, 30 stands and 4 fuel depots once per test
// from a fixed seed, and each test below brackets one operation - a steady tick (and sixty
// more, for wall clock), a committed edit, a hover - with the counter the review named for
// it. A budget that regresses fails HERE, in a run that already exists, rather than as a
// frame-time complaint from play with no counter anywhere near the cause.
//
// NO DRAG-FRAME TEST (Airside.Perf.Scale.DragFrameStaysGeometryOnly, removed in #462 after 1.2 s of the
// suite a run): every assertion it made - no RoadRebuildCensus "Rebuilt:" line, a Geometry rebuild ran,
// zero derived-graph passes - does not depend on the airport's size, since Geometry against Topology is one
// branch in ARoadNetworkActor and the census is gated on bQuiet alone, so a ten-node fixture gives the same
// answer. The behaviours are pinned at that scale. One thing it knew is worth keeping: at scale the two
// runway-exit junctions (a wide runway continuing past a narrow taxiway spur) log RoadMeshBuilder's ear-clip
// fallback ("rim not star-shaped from any apex") on EVERY full surface solve, so a test that wants the census
// line absent has to look for "Rebuilt:" by name, not count log lines.
// ENFORCED BY: Airside.Present.RebuildLogQuietOnDrag, Airside.Present.MutatorNotifiesExactlyOnce.MoveNode.MidDrag,
// Airside.Present.DragNotifiesGeometryOnly
// ---------------------------------------------------------------------------------------

namespace
{
	// FIXED, not re-rolled per test: BuildScale's own seeding only varies which grid slot a
	// stand, depot or route endpoint lands on, never the lattice itself - see its header
	// comment. One constant here is enough for every test in this file to build the bit-
	// identical fixture BuildScale promises.
	constexpr int32 ScaleFixtureSeed = 20260921;

	/**
	 * Aircraft taxiing from the runway exit to every one of the fixture's 30 stands - the
	 * same arrival-to-stand shape every other Airside fixture models. NOT a depot-to-stand
	 * service run: a stand's pose lead-in admits Aircraft (PlaceEntity's PoseRole default,
	 * same as FTestAirport::Build's own stands), while a depot's admits GroundVehicle only
	 * (FuelDepotAnchorTest's own DepotJoinsRoad case) - no single ETraversalClass route
	 * reaches both kinds of pose node in one query. The depots exist in this fixture for
	 * AnchorLinkFindCallCountForTest's own "once per pending link" count (item b below) and
	 * to give the fixture a second entity kind, not as this function's traffic source.
	 *
	 * TestAirframes::GroundOnly(): an Aircraft-class agent with no Climb, so it taxis and
	 * never lands or departs - the "taxi only" shape GroundTrafficTest.cpp uses throughout
	 * for exactly this reason (a dispatched agent that starts already on the ground).
	 *
	 * Returns how many of the StandCount routes actually resolved and dispatched, so a
	 * caller can assert every stand got one rather than silently measuring fewer agents
	 * than the fixture claims to have.
	 */
	int32 DispatchScaleAgents(UGroundTraffic& Traffic, URoadNetwork& Net, const FTestAirport& Airport)
	{
		const FGuidelineNodeId TaxiEntry =
			RouteSearch::FindNearestNode(Net, Airport.ExitAt, ETraversalClass::Aircraft, 5000.0);
		if (!TaxiEntry.IsSet())
		{
			return 0;
		}

		int32 Dispatched = 0;
		for (const FEntityInstanceId& Stand : Airport.Stands)
		{
			// GraphProbe: this fixture measures COST, not policy. It wants the plainest
			// possible search - no runway filter, no penalty, no occupancy term - so that a
			// later change to any errand's row cannot silently move the budget numbers and
			// be read as a performance regression.
			// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
			const FRoutePlan Plan = TestGraph::Probe(Net, TaxiEntry, Airport.Pose(Stand), ETraversalClass::Aircraft);
			if (Plan.IsValid()
				&& Traffic.DispatchAgent(&Net, Plan, TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0) != 0)
			{
				++Dispatched;
			}
		}
		return Dispatched;
	}
}

// ---------------------------------------------------------------------------------------
// Item (a): one Advance at x1 on a settled, 30-agent scale fixture.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScaleSteadyAdvanceIsCheapTest,
	"Airside.Perf.Scale.SteadyAdvanceIsCheap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScaleSteadyAdvanceIsCheapTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FTestAirport Airport = FTestAirport::BuildScale(TestAirframes::Piper(), ScaleFixtureSeed, /*bDerived=*/true, Net);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Dispatched = DispatchScaleAgents(*Traffic, *Net, Airport);
	if (!TestEqual(TEXT("every one of the 30 stands got a dispatched agent"), Dispatched, Airport.Stands.Num()))
	{
		return false;
	}

	// SETTLED FIRST: a freshly-dispatched agent's very first Advance can still be arming a
	// crossing or its first claim pass, which is not the steady "many agents taxiing" state
	// this test means to cost. One tick at a realistic frame time is enough to get there.
	Traffic->Advance(1.0 / 30.0, Net);

	const int32 SampleBefore = Net->SampleGuidelineCallCountForTest();
	FClaimPass::ResetRunCallCountForTest();

	Traffic->Advance(1.0 / 30.0, Net); // one frame, x1, 30 Hz - the plainest real setting

	const int32 SampleDelta = Net->SampleGuidelineCallCountForTest() - SampleBefore;
	const int32 RunCalls = FClaimPass::RunCallCountForTest;
	const int32 Steps = Traffic->GetLastStepsForTest();
	const int32 Agents = Traffic->GetAgentCount();
	const int32 TryClaimCompares = Traffic->OccupancyForTest().GetLastTryClaimComparesForTest();
	const int32 ClaimsInTable = Traffic->OccupancyForTest().GetClaims().Num();

	UE_LOG(LogAirsideTests, Log,
		TEXT("Scale.SteadyAdvance measured (2026-09-21, %d road segments, %d agents): SampleGuideline delta %d, ")
		TEXT("FClaimPass::Run %d (%d agent(s) x %d substep(s)), last TryClaim compares %d of a %d-claim table"),
		Net->GetSegments().Num(), Agents, SampleDelta, RunCalls, Agents, Steps, TryClaimCompares, ClaimsInTable);

	// #171: RouteSearch caches each edge's length on the edge itself precisely so a route
	// search - and an ordinary tick, which asks nothing else of the guideline curve - never
	// re-samples it. See SampleGuidelineCallCountForTest's own comment. Measured 0 on this
	// fixture's steady tick (2026-09-21); any nonzero value is a regression, not drift, so
	// this stays an exact bound rather than a budget.
	TestEqual(TEXT("#171: a steady Advance re-samples no guideline (measured 0, 2026-09-21)"), SampleDelta, 0);

	// #256 (this issue): UGroundTraffic::Arbitrate calls FClaimPass::Run once per agent in
	// rank order, PLUS once more for each agent preempted during that same call (the re-pass
	// over TakePreempted) - so "at most once per agent per substep" is the honest ceiling,
	// not "exactly once", and a legitimate preemption on some future seed or fixture change
	// would otherwise make an exact Agents*Steps bound flaky for a reason that is not the
	// double-dispatch bug this guards against. BUDGET JUST UNDER 2x MEASURED (measured 30 = Agents x
	// Steps exactly, 0 preemptions, 2026-09-21): room for an occasional legitimate re-pass
	// without hiding an accidental SECOND full pass over every agent. STRICT, NOT <=: a double
	// pass over every agent is exactly 2 x Agents x Steps = 60, which a `<=` ceiling of 60 lets
	// through, so the bound was satisfied by the very bug it names (2026-10-01, #463).
	const int32 RunCallBudget = 2 * Agents * Steps;
	TestTrue(FString::Printf(
			TEXT("FClaimPass::Run (%d) stays STRICTLY under 2x agent(s) x substep(s) (%d agent(s) x %d substep(s) x2 = %d)"),
			RunCalls, Agents, Steps, RunCallBudget),
		RunCalls < RunCallBudget);

	// #168 (PR #197's own indexing) IS NOT ASSERTED HERE: TryClaim's cost is bounded by what is ACTUALLY
	// claimed on the one resource being asked about, never by the whole occupancy table. This test read the
	// LAST claim's own compare count against the table's size, which proves a ceiling on whichever claim
	// happened to run last - and a flat scan of the whole table also compares fewer claims than the table
	// holds, so the check passed with the index removed. The measurement that cannot be satisfied by a flat
	// scan, on a table built so one resource's queue is short beside a 400-claim background, is
	// Airside.Model.Occupancy.IndexedCost (#197). The two counts stay in the log line above.

	// WALL CLOCK, folded in from the sixty-ticks test (Airside.Perf.Scale.SixtyAdvancesStayUnderWallClockBudget,
	// removed in #462): the same fixture, seed and dispatch, so a second test rebuilt the 30-agent airport to time
	// what this one has already built. A SMOKE BOUND rather than a benchmark - issue #256's own wording, and
	// the one assertion in this file that measures WALL TIME rather than a call count, so it can catch a future
	// quadratic term nothing here has a counter for yet. It is not a performance target and should never be
	// tightened to chase a specific number. After the counters above are read, because they bracket the one
	// steady tick and these sixty would move them.
	constexpr int32 NumTicks = 60;
	constexpr double Dt = 1.0 / 30.0;
	const double Start = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < NumTicks; ++Index)
	{
		Traffic->Advance(Dt, Net);
	}
	const double Elapsed = FPlatformTime::Seconds() - Start;

	// A GENEROUS CEILING, not a tuned one: 2 seconds for 60 ticks of a 30-agent, ~300-
	// segment airport, measured on the automation runner's own -nullrhi machine (the
	// reference this bound is set against, per Run-AirsideTests.ps1) - comfortably an order
	// of magnitude over what this fixture actually takes (0.004 s, 500x headroom). The point is only that a
	// future quadratic term in the tick loop fails LOUDLY here, in a run that already exists,
	// rather than silently until someone notices frame time climbing in play.
	constexpr double CeilingSeconds = 2.0;

	UE_LOG(LogAirsideTests, Log,
		TEXT("Scale.SixtyAdvances measured (2026-09-21, %d road segments, %d agents): %.3f s for %d Advance call(s), ")
		TEXT("smoke ceiling %.1f s"),
		Net->GetSegments().Num(), Traffic->GetAgentCount(), Elapsed, NumTicks, CeilingSeconds);

	TestTrue(FString::Printf(TEXT("60 Advance calls took %.3f s, under the %.1f s smoke ceiling"), Elapsed, CeilingSeconds),
		Elapsed < CeilingSeconds);

	return true;
}

// ---------------------------------------------------------------------------------------
// Item (b): one committed edit (PlaceNode) on the scale fixture.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScaleCommittedEditCostTest,
	"Airside.Perf.Scale.CommittedEditCost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScaleCommittedEditCostTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	// A NODE FIRST, PURELY TO BRING THE NETWORK INTO BEING - TestGuide::LayRunway's own
	// comment explains why: the facade creates URoadNetwork lazily inside PlaceNode, and
	// BuildScale's own AddNode calls need it to already exist.
	Actor->PlaceNode(FVector2D(-900000.0, -900000.0));
	const FTestAirport Airport = FTestAirport::BuildScale(
		TestAirframes::Piper(), ScaleFixtureSeed, /*bDerived=*/false, Actor->Network.Get());
	Actor->RebuildMesh();

	UGroundTraffic* Model = Actor->GetTraffic()->GetModel();
	const int32 Dispatched = DispatchScaleAgents(*Model, *Actor->Network.Get(), Airport);
	if (!TestEqual(TEXT("every one of the 30 stands got a dispatched agent"), Dispatched, Airport.Stands.Num()))
	{
		return false;
	}

	// #177's OWN TECHNIQUE (ServiceLinkTest.cpp): Gather the pending links on a throwaway
	// COPY of this network state first, so "once per pending link" is checked against a
	// number this test measured independently, not one it assumes (30 stands + 4 depots
	// might not be 34 pending links if a stand's own definition carries more than one
	// anchor - Gather is the one place that actually knows).
	int32 PendingCount = 0;
	{
		URoadNetwork* Snapshot = NewObject<URoadNetwork>(GetTransientPackage());
		FTestAirport::BuildScale(TestAirframes::Piper(), ScaleFixtureSeed, /*bDerived=*/false, Snapshot);
		TestGraph::Derive(*Snapshot);

		TArray<FPendingLink> Pending;
		TSet<FGuidelineNodeId> AnchorNodes;
		FAnchorLink::Gather(*Snapshot, FAnchorLink::DefaultMaxLeadIn, FAnchorLink::DefaultServiceLinkRadius,
			Pending, AnchorNodes);
		PendingCount = Pending.Num();
	}
	if (!TestTrue(TEXT("the scale fixture has links waiting to be resolved on its next rebuild"), PendingCount > 0))
	{
		return false;
	}

	RouteSearch::ResetNodeVisitCountForTest();
	UAirsideSettings::ResetResolveLargestServiceVehicleCallCountForTest();
	const int32 AnchorLinkBefore = Actor->Network->AnchorLinkFindCallCountForTest();

	// THE MEASURED EDIT: an isolated node, far off the grid, so its own solve and mesh cost
	// stay negligible next to what the REBUILD it triggers must redo across the whole
	// fixture - the guideline derive, every agent's route re-resolve, and every stand and
	// depot's anchor re-link.
	const int32 NewNode = Actor->PlaceNode(FVector2D(900000.0, 900000.0));

	const int32 Visits = RouteSearch::NodeVisitCountForTest();
	const int32 ResolveCalls = UAirsideSettings::ResolveLargestServiceVehicleCallCountForTest;
	const int32 AnchorLinkCalls = Actor->Network->AnchorLinkFindCallCountForTest() - AnchorLinkBefore;
	const int32 NumAgents = Model->GetAgentCount();
	const int32 NumNodes = Actor->Network->GetGuidelineNodes().Num();
	const int64 WorstCase = static_cast<int64>(NumAgents) * (NumNodes + 1) * NumNodes;

	UE_LOG(LogAirsideTests, Log,
		TEXT("Scale.CommittedEdit measured (2026-09-21, %d road segments): FindNearestNode visits %d ")
		TEXT("(%d agents, %d nodes, worst case %lld), AnchorLinkFind calls %d of %d pending, ResolveLargestServiceVehicle %d"),
		Actor->Network->GetSegments().Num(), Visits, NumAgents, NumNodes, WorstCase, AnchorLinkCalls, PendingCount, ResolveCalls);

	if (!TestTrue(TEXT("the new node was actually placed"), NewNode != INDEX_NONE))
	{
		return false;
	}

	// #172 (PR #207's own bound): FindNearestNode's indexed rebuild visits a small fraction
	// of A*S*N (agents x remaining steps x nodes), never the whole graph per re-resolve.
	// THE BOUND IS NUMNODES, NOT A*S*N/10 (#463): the /10 margin #207's own GraphRebuildNodeVisits test uses
	// is 19.6M here against 256 measured, and the O(N)-per-call code the index replaced - about 256 x 2559 =
	// 655K visits at this fixture - sits far under it, so the old bound passed with the index gone. All the
	// agents' re-resolves together visit fewer nodes than the graph HAS (measured 256 of 2559, 2026-10-01):
	// one scan per agent alone is 30 x 2559 and fails it.
	TestTrue(FString::Printf(TEXT("FindNearestNode visits (%d) are fewer than the graph's nodes (%d), let alone A*S*N (%lld)"),
			Visits, NumNodes, WorstCase),
		Visits < NumNodes);

	// #177: one ILinkFinder::Find dispatch per pending link, not a second pass over the
	// same set (the defect #177 fixed would show as up to double PendingCount here).
	TestEqual(FString::Printf(TEXT("one ILinkFinder::Find per pending link (%d)"), PendingCount),
		AnchorLinkCalls, PendingCount);

	// #232: the largest service vehicle is resolved exactly once per rebuild, not once per
	// arm/link/pair inside it.
	TestEqual(TEXT("#232: ResolveLargestServiceVehicle runs once per rebuild"), ResolveCalls, 1);

	return true;
}

// ---------------------------------------------------------------------------------------
// Item (d): one hover context on the scale fixture.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScaleHoverClaimsFewNodesTest,
	"Airside.Perf.Scale.HoverClaimsFewNodes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScaleHoverClaimsFewNodesTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	// NOT DERIVED: a hover asks the ROAD network's node claims, and the guideline graph Derive would add
	// (the bulk of this test's 0.58 s) is never read by it. The count measured below is the same either way.
	FTestAirport::BuildScale(TestAirframes::Piper(), ScaleFixtureSeed, /*bDerived=*/false, Net);

	FRoadSnapSettings Settings; // level defaults, exactly what a real hover uses
	FRoadSnapChain Chain;

	FRoadNetworkSolver::ResetNodeClaimsCallCountForTest();

	// OPEN GROUND IN THE MIDDLE OF THE GRID - no node under the cursor, so the chain must
	// ask every candidate #167's cheap reject cannot already rule out on distance before it
	// can answer Free. The costliest ordinary hover, not the cheapest (a cursor sitting
	// exactly on a node resolves from the fixed-radius check alone, with no solve at all).
	const FVector2D MidGrid(60000.0, -30000.0);
	Chain.Resolve(*Net, MidGrid, Settings);

	const int32 Calls = FRoadNetworkSolver::NodeClaimsCallCountForTest;
	const int32 NodeCount = Net->GetNodes().Num();

	UE_LOG(LogAirsideTests, Log,
		TEXT("Scale.HoverClaimsFewNodes measured (2026-09-21, %d road segments): %d NodeClaims call(s) of %d road nodes"),
		Net->GetSegments().Num(), Calls, NodeCount);

	// #167: MaxPossibleNodeClaimReach rejects most nodes from data already on hand, with no
	// junction solve at all - a hover on a ~300-segment airport must still touch only a
	// small fraction of them, the issue's own "fewer than 5%".
	TestTrue(FString::Printf(TEXT("one hover claims %d of %d road node(s) (%.1f%%), under the 5%% budget"),
			Calls, NodeCount, NodeCount > 0 ? 100.0 * Calls / NodeCount : 0.0),
		Calls < NodeCount * 0.05);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
