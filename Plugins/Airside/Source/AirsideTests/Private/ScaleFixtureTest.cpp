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
// from a fixed seed, and each test below brackets one operation - a steady tick, a
// committed edit, a drag frame, a hover, sixty ticks' wall clock - with the counter the
// review named for it. A budget that regresses fails HERE, in a run that already exists,
// rather than as a frame-time complaint from play with no counter anywhere near the cause.
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
			FRouteQuery Query;
			Query.Errand = ERouteErrand::GraphProbe;
			Query.Policy = FRoutePolicy::For(Query.Errand);
			Query.Start = TaxiEntry;
			Query.Goal = Airport.Pose(Stand);
			Query.Class = ETraversalClass::Aircraft;
			const FRoutePlan Plan = RouteSearch::Find(Net, Query);
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
	// double-dispatch bug this guards against. BUDGET AT 2x MEASURED (measured 30 = Agents x
	// Steps exactly, 0 preemptions, 2026-09-21): room for an occasional legitimate re-pass
	// without hiding an accidental SECOND full pass over every agent, which would still
	// clear this bound by nowhere near as tight a margin for any double-digit agent count.
	const int32 RunCallBudget = 2 * Agents * Steps;
	TestTrue(FString::Printf(
			TEXT("FClaimPass::Run (%d) stays within 2x agent(s) x substep(s) (%d agent(s) x %d substep(s) x2 = %d)"),
			RunCalls, Agents, Steps, RunCallBudget),
		RunCalls <= RunCallBudget);

	// #168 (PR #197's own indexing): TryClaim's cost is bounded by what is ACTUALLY claimed
	// on the one resource being asked about, never by the whole occupancy table - this reads
	// the LAST claim's own compare count, so it proves the ceiling on whichever claim
	// happened to run last, not the aggregate (#197's own IndexedCost test measures that).
	TestTrue(FString::Printf(TEXT("last TryClaim compared %d claim(s), under the %d-claim table"),
			TryClaimCompares, ClaimsInTable),
		TryClaimCompares < ClaimsInTable);

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
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Snapshot);
		FRoadGuidelineBuilder::Build(*Snapshot, Solved, UAirsideSettings::ResolveLargestServiceVehicle());

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
	// Same /10 margin #207's own GraphRebuildNodeVisits test uses - it rules out the O(N)-
	// per-call code the index replaced, not a tighter constant nobody has measured yet.
	TestTrue(FString::Printf(TEXT("FindNearestNode visits (%d) are well under A*S*N (%lld)"), Visits, WorstCase),
		static_cast<int64>(Visits) < WorstCase / 10);

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
// Item (c): one drag frame (Geometry) on the scale fixture.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScaleDragFrameStaysGeometryOnlyTest,
	"Airside.Perf.Scale.DragFrameStaysGeometryOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScaleDragFrameStaysGeometryOnlyTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	Actor->PlaceNode(FVector2D(-900000.0, -900000.0));
	const FTestAirport Airport = FTestAirport::BuildScale(
		TestAirframes::Piper(), ScaleFixtureSeed, /*bDerived=*/false, Actor->Network.Get());
	Actor->RebuildMesh();

	if (!TestTrue(TEXT("the sample grid node resolves"), Airport.SampleGridNode.IsSet()))
	{
		return false;
	}

	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade))
	{
		return false;
	}

	const int32 RebuildCountBefore = Actor->RebuildCountForTest();
	const int32 TopologyRebuildCountBefore = Actor->TopologyRebuildCountForTest();

	// MoveNode's second parameter is the ABSOLUTE destination, not a delta (RoadEditFacade::
	// MoveNode's own "To" - RoadRebuildLogQuietTest.cpp's own drag moves B from (10000,0) to
	// (10500,500), a small step FROM its own position, never a bare offset), so the target
	// here is the node's own live position plus a small nudge, not the nudge alone.
	const FRoadNode* LiveNode = Actor->Network->GetNode(Airport.SampleGridNode);
	if (!TestNotNull(TEXT("the sample grid node is live"), LiveNode))
	{
		return false;
	}
	const FVector2D DragTo = LiveNode->Position + FVector2D(500.0, 500.0);

	// #216's own spy, reused rather than re-hand-copied (issue #216d) - watches LogRoadMesh
	// at exactly Log verbosity across the bracketed window.
	FLogLineSpy DragSpy(FName(TEXT("LogRoadMesh")));
	GLog->AddOutputDevice(&DragSpy);
	Facade->BeginInteractiveEdit(TEXT("drag an interior taxiway node"));
	const bool bMoved = Actor->MoveNode(Airport.SampleGridNode.Index, DragTo);
	GLog->RemoveOutputDevice(&DragSpy);

	// MEASURED HERE, BEFORE EndInteractiveEdit - RoadRebuildLogQuietTest.cpp's own ordering,
	// for the same reason: EndInteractiveEdit fires the one Topology catch-up notify a
	// committed drag owes the derived graph, so reading these counters after it would count
	// the COMMIT's rebuild as though the drag frame itself had run it.
	const int32 RebuildCountAfter = Actor->RebuildCountForTest();
	const int32 TopologyRebuildCountAfter = Actor->TopologyRebuildCountForTest();

	// DISCARDED, NOT KEPT: this test measures the DRAG FRAME only (issue #178's own
	// scenario, up to 60 times a second while a node is held), not the commit - #216's
	// RebuildLogQuietOnDragTest already pins that the commit still logs in full.
	Facade->EndInteractiveEdit(/*bKeep=*/false);

	if (!TestTrue(TEXT("the drag frame moves the node"), bMoved))
	{
		return false;
	}

	// NAMED, NOT COUNTED: at scale this fixture's runway-exit junctions (2 of the 168 road
	// nodes - a wide runway continuing straight past a much narrower taxiway spur) hit
	// RoadMeshBuilder's ear-clip fallback ("rim not star-shaped from any apex") on EVERY
	// full surface solve, including a Geometry-only one, because a Geometry rebuild re-
	// triangulates every junction, not only the one that moved - a real LogRoadMesh line
	// at scale that a ten-node fixture never reaches, but a DIFFERENT log site
	// (RoadMeshBuilder.cpp) from the one issue #178/#256 name. The issue's own wording is
	// specific - "RoadRebuildCensus::Log zero times" - so this checks for THAT line by name
	// (RoadRebuildCensus::Log's own "Rebuilt:" line, the same substring
	// RoadRebuildLogQuietTest.cpp's FLogRoadMeshLogSpy keys on), not a bare zero-line count
	// that an unrelated, pre-existing solver warning would fail regardless of this issue.
	const bool bSawCensusLine = DragSpy.CapturedLines.ContainsByPredicate(
		[](const FString& Line) { return Line.Contains(TEXT("Rebuilt:")); });

	UE_LOG(LogAirsideTests, Log,
		TEXT("Scale.DragFrame measured (2026-09-21, %d road segments): LogRoadMesh lines %d (census line seen: %s), ")
		TEXT("RebuildCount %d -> %d, TopologyRebuildCount %d -> %d. Captured: %s"),
		Actor->Network->GetSegments().Num(), DragSpy.Count, bSawCensusLine ? TEXT("true") : TEXT("false"),
		RebuildCountBefore, RebuildCountAfter, TopologyRebuildCountBefore, TopologyRebuildCountAfter,
		*FString::Join(DragSpy.CapturedLines, TEXT("; ")));

	// #178: RoadRebuildCensus::Log's own "Rebuilt:" line does not fire on a Geometry
	// (drag-frame) rebuild, at scale as much as at ten nodes - the census is what a drag
	// frame must not pay for, whatever the airport's size.
	TestFalse(TEXT("#178: a drag frame on a ~300-segment airport does not log RoadRebuildCensus's 'Rebuilt:' line"),
		bSawCensusLine);

	// A GEOMETRY REBUILD STILL RAN (RebuildCount bumps on every RebuildMeshForChange call,
	// including Geometry) ...
	TestTrue(TEXT("the drag frame still ran a Geometry rebuild"), RebuildCountAfter > RebuildCountBefore);

	// ... but issue #256's own measurement: the DERIVED-graph pass - guideline graph, anchor
	// links, plots, traffic re-resolve - runs zero times on a drag frame, at scale as much
	// as at ten nodes (issue #165's own TopologyRebuildCount exists for exactly this split).
	TestEqual(TEXT("#256: the derived-graph pass does not run on a drag frame at scale"),
		TopologyRebuildCountAfter, TopologyRebuildCountBefore);

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
	FTestAirport::BuildScale(TestAirframes::Piper(), ScaleFixtureSeed, /*bDerived=*/true, Net);

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

// ---------------------------------------------------------------------------------------
// Wall-clock guard: 60 Advance calls on the scale fixture, a smoke bound rather than a
// benchmark - issue #256's own wording. This is the one assertion in this file that
// measures WALL TIME rather than a call count, so it can catch a future quadratic term
// nothing here has a counter for yet; it is not a performance target and should never be
// tightened to chase a specific number.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScaleSixtyAdvancesStayUnderWallClockBudgetTest,
	"Airside.Perf.Scale.SixtyAdvancesStayUnderWallClockBudget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScaleSixtyAdvancesStayUnderWallClockBudgetTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FTestAirport Airport = FTestAirport::BuildScale(TestAirframes::Piper(), ScaleFixtureSeed, /*bDerived=*/true, Net);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Dispatched = DispatchScaleAgents(*Traffic, *Net, Airport);
	if (!TestEqual(TEXT("every one of the 30 stands got a dispatched agent"), Dispatched, Airport.Stands.Num()))
	{
		return false;
	}

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
	// of magnitude over what this fixture actually takes. The point is only that a future
	// quadratic term in the tick loop fails LOUDLY here, in a run that already exists,
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

#endif // WITH_DEV_AUTOMATION_TESTS
